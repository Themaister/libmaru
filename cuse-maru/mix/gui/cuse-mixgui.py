#!/usr/bin/env python

from gi.repository import Gtk, GObject
import socket, os, sys

class Connection:
   def __init__(self, sock):
      self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
      self.sock.connect(sock)

   def set_volume(self, stream, vol):
      command = "SETPLAYVOL {} {}".format(stream, vol)
      message = "MARU{:4} {}".format(len(command) + 1, command)
      self.sock.send(message.encode())

      reply = self.sock.recv(8)
      length = int(reply.decode().split(" ")[-1])

      reply = self.sock.recv(length)

   def get_volume(self, stream):
      command = "GETPLAYVOL {}".format(stream)
      message = "MARU{:4} {}".format(len(command) + 1, command)
      self.sock.send(message.encode())

      reply = self.sock.recv(8)
      length = int(reply.decode().split(" ")[-1])

      reply = self.sock.recv(length)
      vol = int(reply.decode().split(" ")[-1])
      return vol

class Control(Gtk.VBox):
   def __init__(self, conn, i):
      Gtk.Box.__init__(self)
      self.pack_start(Gtk.Label("Stream #{}".format(i)), False, True, 10)
      self.scale = Gtk.VScale()
      self.scale.set_range(0, 100)
      self.scale.set_value(0)
      self.scale.set_size_request(-1, 300)
      self.scale.set_property("inverted", True)
      self.scale.set_sensitive(False)
      self.pack_start(self.scale, True, True, 10)
      self.i = i
      self.conn = conn

      self.scale.connect("value-changed", self.vol_change)
      GObject.timeout_add_seconds(1, self.update_timer)

   def vol_change(self, widget):
      self.conn.set_volume(self.i, int(self.scale.get_value()))

   def update_timer(self):
      try:
         self.scale.set_value(self.conn.get_volume(self.i))
         self.scale.set_sensitive(True)
      except:
         self.scale.set_sensitive(False)
         self.scale.set_value(0)

      GObject.timeout_add_seconds(1, self.update_timer)

class Window(Gtk.Window):
   def __init__(self):
      Gtk.Window.__init__(self, title = "MARU Volume Control")
      self.conn = Connection("/tmp/marumix")
      self.set_border_width(5)

      box = Gtk.HBox()
      for i in range(8):
         box.pack_start(Control(self.conn, i), True, True, 10)
      self.add(box)

if __name__ == '__main__':
   win = Window()
   win.connect("delete-event", Gtk.main_quit)
   win.show_all()
   Gtk.main()

