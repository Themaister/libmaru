# libmaru

libmaru is a library that can take control of a USB audio device, and use it directly as an audio playback device.
The implementation resides entirely in userspace, and uses libusb 1.0 to communicate with the device.

## MARUSS

MARUSS is a project that implements a USB sound card on an Altera DE2 development board.
It features hardware mixing and resampling.

The public project report is found [here](https://github.com/downloads/Themaister/libmaru/report.pdf).
The appendices for the report as well as source code for MARUSS can be found [here](https://github.com/downloads/Themaister/libmaru/MARUSS.zip).

## Licensing

libmaru is licensed under LGPLv2.1+ (same as libusb).

## Dependencies

To build libmaru, you need to satisfy these dependencies:<br/>
   - Linux 2.6.22+<br/>
   - FUSE /w CUSE support<br/>
   - libusb 1.0+<br/>

## Building and installing

After dependencies have been satisfied, it should be sufficient to build and install with:<br/>
<tt>make</tt><br/>
<tt>make install PREFIX=$PREFIX</tt><br/>

## Building documentation

The public libmaru API is documented with doxygen.
To build documentation, doxygen must be installed. Build documentation using:<br/>
<tt>doxygen Doxyfile</tt>

Documentation will be installed to doc/.

## Notes on permissions

To communicate with the USB subsystem, write access to USB nodes in usbfs is required.
It might be necessary to grant write permissions to /dev/bus/usb for the processes that use libmaru.

# cuse-maru

cuse-maru is a project that implements a subset of Open Sound System in userspace using CUSE.
It uses libmaru to pass audio to the USB audio card. It supports multiple hardware streams if the audio device does.

## Licensing

cuse-maru is licensed under GPLv3+.

## Building and installing

<tt>cd cuse-maru</tt><br/>
<tt>make</tt><br/>
<tt>make install PREFIX=$PREFIX</tt><br/>

## Running cuse-maru

To run cuse-maru, cuse module must be loaded. /dev/cuse must also be writable by cuse-maru process.
This can be set up with an udev rule, such as:

<tt>KERNEL=="cuse", MODE="0660", GROUP="audio"</tt><br/>

By default, cuse-maru will create the OSS device in /dev/maru.
To automatically set permissions to this device, a similar rule can be created as such:

<tt>KERNEL=="maru", MODE="0660", GROUP="audio"</tt><br/>

To add an automatic symlink to the device, i.e. /dev/dsp, it can be done as such:
<tt>KERNEL=="maru", MODE="0660", SYMLINK+="dsp", GROUP="audio"</tt><br/>

## Incompatibilities

   - cuse-maru is fairly compatible with the OSSv3 API, and also supports cherry picked functionality from OSSv4. Most of the obscure calls are unsupported.
   - mmap() is not supported (as one cannot mmap() an USB device, and there is no way to know exactly the internal buffer pointers, rendering mmap() kinda useless anyways).
   - It only supports /dev/dsp interface. /dev/mixer is not supported.
   - cuse-maru only supports playback. open() calls with O_RDONLY or O_RDWR will raise EACCES.
   - To control master volume of /dev/maru, it is possible (not standard) to use the OSSv4 SNDCTL_DSP_SETPLAYVOL/SNDCTL_DSP_GETPLAYVOL ioctl() calls on a newly opened device to control it. In cuse-maru/volume, a simple CLI tool to do this is provided. Build instructions are identical to cuse-maru.

## Audio conversions

cuse-maru is a direct hardware bridge, and does not perform sample rate conversion,
channel up/down-sampling or sample format conversions.
If this is needed, an external sound server is recommended.

If only simple sample rate conversion is needed, cuse-mix can be used.

# cuse-mix

cuse-mix is a CUSE OSS device that performs sample rate conversions, mixes streams, and passes them along to a different OSS device (i.e. a device governed by cuse-maru).
If USB card doesn't support resampling and mixing, cuse-mix can be a viable alternative.

## Building and installing

<tt>cd cuse-maru/mix</tt><br/>
<tt>make</tt><br/>
<tt>make install PREFIX=$PREFIX</tt><br/>

## Running cuse-mix

Similar to cuse-maru.
If cuse-mix is being used as the primary audio device, it might be an idea to symlink this to /dev/dsp rather than cuse-maru.
By default, cuse-mix will create a device in /dev/marumix.

## Differences in implementation from cuse-maru

   - Opening a device and using SNDCTL_DSP_SETPLAYVOL/SNDCTL_DSP_GETPLAYVOL directly does not work the same way as cuse-maru does. SETPLAYVOL/GETPLAYVOL sets the playing volume as expected on the stream.
   - To control volume per-stream and master volume, a simplistic Python3/GTK GUI is provided in cuse-maru/mix/gui/cuse-mixgui.py.

