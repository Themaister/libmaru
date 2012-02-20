#!/usr/bin/env python

from gi.repository import Gtk, GObject
import socket, os, sys, fcntl, array, struct

class Connection:
   def __init__(self, sock):
      self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
      self.sock.connect(sock)

   def get_reply(self):
      reply = self.sock.recv(8)
      length = int(reply.decode().split(" ")[-1])
      return self.sock.recv(length)

   def set_volume(self, stream, vol):
      command = "SETPLAYVOL {} {}".format(stream, vol)
      message = "MARU{:4} {}".format(len(command) + 1, command)
      self.sock.send(message.encode())

      return self.get_reply()

   def get_volume(self, stream):
      command = "GETPLAYVOL {}".format(stream)
      message = "MARU{:4} {}".format(len(command) + 1, command)
      self.sock.send(message.encode())

      reply = self.get_reply()
      vol = int(reply.decode().split(" ")[-1])
      return vol

   def get_name(self, stream):
      command = "GETNAME {}".format(stream)
      message = "MARU{:4} {}".format(len(command) + 1, command)
      self.sock.send(message.encode())

      return self.get_reply().decode().split(" ")[-1]

class MasterControl(Gtk.HBox):
   def __init__(self, path):
      Gtk.HBox.__init__(self)
      self.pack_start(Gtk.Label("Master"), False, True, 30)

      self.scale = Gtk.HScale()
      self.scale.set_range(0, 100)
      self.scale.set_value(0)
      self.scale.set_size_request(200, -1)
      self.scale.set_round_digits(0)
      self.scale.set_sensitive(False)
      self.scale.connect("value-changed", self.vol_change)

      self.setplayvol = 0xc0045018 # IOCTL stuff
      self.getplayvol = 0x80045018 # IOCTL stuff

      self.fd = open(path, 'wb')

      self.pack_start(self.scale, True, True, 20)

      self.update_timer()

   def set_volume(self, vol):
      buf = array.array('i', [vol])
      fcntl.ioctl(self.fd.fileno(), self.setplayvol, buf)

   def get_volume(self):
      buf = array.array('i', [0])
      fcntl.ioctl(self.fd.fileno(), self.getplayvol, buf, 1)
      return struct.unpack('i', buf)[0] & 0xff

   def vol_change(self, widget):
      self.set_volume(int(self.scale.get_value()))

   def update_timer(self):
      try:
         self.scale.set_value(self.get_volume())
         self.scale.set_sensitive(True)
      except:
         self.scale.set_sensitive(False)
         self.scale.set_value(0)

      GObject.timeout_add_seconds(5, self.update_timer)

class Control(Gtk.VBox):
   def __init__(self, conn, i):
      Gtk.VBox.__init__(self)
      self.scale = Gtk.VScale()
      self.process = Gtk.Label()
      self.pack_start(self.process, False, True, 10)
      self.scale.set_range(0, 100)
      self.scale.set_value(0)
      self.scale.set_size_request(-1, 300)
      self.scale.set_round_digits(0)
      self.set_size_request(25, -1)
      self.scale.set_inverted(True)
      self.scale.set_sensitive(False)
      self.pack_start(self.scale, True, True, 10)
      self.i = i
      self.conn = conn

      self.scale.connect("value-changed", self.vol_change)
      self.update_timer()

   def vol_change(self, widget):
      self.conn.set_volume(self.i, int(self.scale.get_value()))

   def update_timer(self):
      try:
         self.scale.set_value(self.conn.get_volume(self.i))
         self.process.set_text(self.conn.get_name(self.i))
         self.scale.set_sensitive(True)
      except:
         self.scale.set_sensitive(False)
         self.process.set_text("")
         self.scale.set_value(0)

      GObject.timeout_add(100, self.update_timer)

class Window(Gtk.Window):
   def __init__(self):
      Gtk.Window.__init__(self, title = "MARU Volume Control")
      self.conn = Connection("/tmp/marumix")
      self.set_border_width(5)

      vbox = Gtk.VBox()

      vbox.pack_start(MasterControl("/dev/maru"), False, True, 3)
      vbox.pack_start(Gtk.HSeparator(), False, True, 3)

      box = Gtk.HBox()

      box.pack_start(Control(self.conn, 0), True, True, 3)
      box.pack_start(Gtk.VSeparator(), False, True, 3)
      box.pack_start(Control(self.conn, 1), True, True, 3)
      box.pack_start(Gtk.VSeparator(), False, True, 3)
      box.pack_start(Control(self.conn, 2), True, True, 3)
      box.pack_start(Gtk.VSeparator(), False, True, 3)
      box.pack_start(Control(self.conn, 3), True, True, 3)

      vbox.pack_start(box, True, True, 0)
      self.add(vbox)

if __name__ == '__main__':
   win = Window()
   win.connect("delete-event", Gtk.main_quit)
   win.show_all()
   Gtk.main()

