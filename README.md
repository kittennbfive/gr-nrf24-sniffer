# gr-nrf24-sniffer

## What is this?
gr-nrf24-sniffer is a tool to receive and decode wireless traffic from nRF24L01(+) modules (or older nRF24xxx) using [GNU Radio](https://www.gnuradio.org/) 3.8 and a [HackRF One](https://greatscottgadgets.com/hackrf/one/) (or another compatible SDR). The *receiver* is "written" in GNU Radio and outputs raw samples to a named pipe / FIFO, the *decoder* that reads from this named pipe is written in C. It was created because i needed a way to debug a nRF24L01(+) wireless link. This tool is for Linux only, tested on Debian 11.

## Licence and Disclaimer
This project is licenced under the AGPLv3+ and provided WITHOUT ANY WARRANTY! Note that while the C-code shouldn't be too bad, the GNU Radio-stuff could benefit from some improvements. This tool (GNU Radio) is really powerful but not easy to master, also because of the somewhat sparse documentation. However for me this tool works (YMMV).

## How to compile the decoder
As simple as `gcc -o nrf-decoder -O3 nrf-decoder.c`. No particular dependencies. As i said, Linux only, but maybe with Cygwin or something like this it can work on Windows. Please don't ask me for support for this however.

## How to use
Compile the decoder. Make sure your SDR is connected and switched on. Create a named pipe called `fifo_grc` in `/tmp` (`cd /tmp && mkfifo fifo_grc`). Open `nrf-receiver.grc` with Gnuradio 3.8 (might also work with 3.9, untested; will not work with 3.7). Then **first** start the decoder using `cd /tmp && cat $fifo_grc | ./nrf-decoder $options` (see below for `$options`) and **then** start the receiver from inside GNU Radio (or directly start the generated Python3 code). If you forget to start the decoder first the GUI of the receiver will not show up!  
  
Once the GUI is up and running you need to select the nRF24 speed (250kbps/1Mbps/2Mbps). Note that if you need 2Mbps you first have to select 1Mbps and then 2Mbps, if you directly go from 250kbps to 2Mbps some internal test will fail and the GUI crashes... You also need to select the channel on which you want to receive. After that you can tweak some parameters of the receiver inside the GUI to get a good decoded signal on the virtual oscilloscope at the bottom of the screen. If everything looks good you can start piping data to the decoder via the named pipe by ticking the "Write to file/pipe" checkbox. You now should see some output from the decoder (depending on the selected options).  
  
Please note that using GNU Radio inside a VM with USB-passtrough is not recommanded. It may work or may not work or only work sometimes and produce weird errors including nasty crashes of Python3.

## Options of the decoder
The order of the options does not matter.
### general options
* `--spb $number` **Mandatory!** The number of samples spit out by the receiver for each bit. The value depends on the selected speed and is displayed inside the GUI. It is 8 (samples/bit) for 250kbps and 1Mbps or 6 for 2Mbps. **Note that this value must be correct or you won't see any valid packets!** Also note that due to the way the decoder is implemented this value must be somewhat high, let's say bigger or equal to 4 (?) to make it work. This is not a problem with a HackRF One that can go up to 20Msps sampling rate but could be a problem with other SDR.
* `--sz-addr $number` **Mandatory!** The size of the address-field inside the packets. This can be between 3 and 5 bytes. ($number!=5 untested)
* `--sz-payload $number` The size of the payload inside data-packets. This can be from 1 to 32 bytes and is mandatory unless you specify `--dyn-lengths`.
* `--sz-ack-payload $number` The size of payload inside *ACK*-packets. This can be from 0 (no payload) to 32 bytes and is mandatory unless you specify `--dyn-lengths`.  
   
*If `--dyn-lengths` is specified or `--sz--payload` and `--sz-ack-payload` have the same value there is no way for the decoder to distinguish between data-packets and ACK-packets with payload so a more generic output format will be used.*
### display/output options
By default the decoder will not show all the packet details but only a summary and will not spit out the packet-payload as raw bytes. You can change this using these options:
* `--disp [verbose|retransmits|none]` Show everything|just retransmits|nothing (printed to stderr). Note that option 2 requires the decoder to be able to distinguish between data-packets and ACK-packets, so `--dyn-lengths` is not allowed and `--sz-payload` must be different from `--sz-ack-payload`.
* `--dump-payload [data|ack|all]` Dump payload of data-packets|of ack-packets|of both packets on stdout. Note that the latter two options cannot be combined with `--mode-compatibility` and option 1 and 2 requires the decoder to be able to distinguish packets (see just above).
### other options
* `--mode-compatibility` Compatibility-mode for nRF2401A, nRF2402, nRF24E1 and nRF24E2 (no packet control field, no auto-ack, no auto-retransmit). See datasheet of the nRF24L01+ section 7.10. For nRF24L01+ you don't need this unless you configured your nRF specifically for compatibility (EN_AA=0x00, ARC=0, speed 250kbps or 1Mbps).
* `--dyn-lengths` Tell the decoder that data-packets and/or ACK-packets have a dynamic payload length specified inside the packet control field. For fixed payload-size use `--sz-payload $number` and `--sz-ack-payload $number` instead to allow the decoder to detect the type of a packet (data or ACK). 
* `--crc16` Use this if your wireless link uses a 2 byte CRC instead of the default 1 byte. I recommand using this with your own projects for better error-detection / less false positives (bit CRCO in register CONFIG set).
* `--filter-addr $addr_in_hex` Only consider packets for the specified address (in hex with or without leading "0x"). By default the decoder is in promiscous-mode. The size of the specified address (number of bytes) must match `--sz-addr`.

## Prior work
* \[Cyber Explorer\] did some work on sniffing nRF24L01+ (and BLE) communication with an RTL-SDR and some additional hardware in 2014. You can check it out here: http://blog.cyberexplorer.me/2014/01/sniffing-and-decoding-nrf24l01-and.html
* Other people have probably worked on this too.

## Some random notes/comments/...
* Thanks to Nordic Semiconductor for describing the packet-format of their chips inside the public datasheets!
* The cheap nRF24L01+ modules you can get from places like Aliexpress seem to contain fake chips, at least sometimes. From my limited experiments some of those modules are not transmitting exactly on the specified channel/frequency. Just use your SDR to check for this if you have trouble getting a (stable) wireless link. There is a test mode (constant carrier) on the nRF24 as decribed in the datasheet (last page), it makes checking the frequency/channel really easy.
* The decoder does not know the actual on air data rate, it only uses "samples per bit" (`--spb`) for which the correct number must be specified.
* Note that the payload_length-field inside the PCF is only valid if dynamic payload length is enabled. This means there is no way to guess the payload-length of some random transmission, except by try and error while checking for correct CRC.
* If you wonder about that big spike at the center of the spectrum-plot see explanations here: https://hackrf.readthedocs.io/en/latest/faq.html#what-is-the-big-spike-in-the-center-of-my-received-spectrum
* If you need a HackRF One be aware that this project is fully Open Source so they are chinese "clones" that seems to work fine too and are much cheaper. Of course if you can afford it buy an original HackRF One to support the project!
* You can save data from the receiver to a file by modifying the file sink component in GNU Radio and then decode it later using `cat $file | ./nrf-decoder $options`, although the timestamps won't be correct.
* If you need to change some option for the decoder untick the "Write to file/pipe" box in GNU Radio first **before** killing the decoder with Ctrl+C. If you don't do it this way GNU Radio will complain about overflows ("O" written in the console at the bottom of the screen) and stop working. Just restart the GUI and and don't forget to configure it correctly again (speed, channel, ...)!
* I know it might be considered bad practice but i deliberately put all the C-code inside a single file to keep things simple.
* If you want to process the packet-payload directly you can use something like `cat fifo_grc | ./nrf-decoder [...] --disp none --dump-payload [data|ack|all] | ./your_tool`.
* You can click on the oscilloscope view with the middle mouse button to get a menu to change the number of displayed samples and lots of other stuff.
