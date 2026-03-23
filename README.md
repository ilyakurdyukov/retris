## Retris

A tetromino game with the following features:

- demo recording and playback
- time rewind
- automatic playback of recorded demos after a gameover
- gamepad support
- Linux terminal output
- Zengge/Surplife 20x20 LED smart curtain output

Was developed specifically for the purpose of outputting to a 20x20 LED smart curtain (which use the Surplife app), so the resolution in the Linux terminal is the same.

Has no relation to the game of the same name distributed on Steam (which is not a tetromino at all).

Can run on both regular PCs and small development boards, tested under Armbian on Raspberry Pi Zero 2W, Orange Pi One and Orange Pi Zero Plus2 H3. But for the development board, you might need a decent USB Bluetooth adapter. For example, the built-in Bluetooth on the Raspberry Pi Zero 2W produces a very low frame rate when updating smart curtain.

### Build

Simply use `make`.

If you don't need smart curtain support, add the `CURTAIN=0` option to `make`.

Since the game uses Linux terminal escape codes, it cannot run on Windows. It might work in the MSYS2 terminal (not tested).

### Options

`--refresh 5..100` - set the refresh rate in ms (default 20)  
`--next [-]0..3` - how many next pieces to show, a negative value inverts the order (default 1)  
`--record demo.rec` - record a demo file  
`--play demo.rec` - play a demo file  
`--seekend N` - seek N seconds from the end of the demo  
`--dir path` - play demos from this directory (default "replays")  
`--rewind 1..120` - rewind length in seconds (default 10)  
`--testdemo` - show demo info (specified by `--play` option)  
`--notermgfx` - disable graphics output to the terminal  
`--color {rgb|simple|no}` - set terminal colors (default "simple")  

Joystick options:

`--js /dev/input/jsN` - specify the device  
`--js_thr 0x4fff{,0x2fff}` - axis on/off thresholds  
`--js_rep N` - button repeat rate in ms (default 100)  

Smart curtain options:

`--curtain_mac XX:XX:XX:XX:XX:XX` - specify the MAC address of the Zengge Smart Curtain  
`--curtain_retry N` - retry after N ms if connection is lost (disabled by default)  
`--curtain_refresh N` - curtain refresh rate in ms (default 67)  
`--curtain_verbose N` - verbosity level for the Bluetooth code  

### Examples

Run the game on a smart curtain without displaying graphics in the terminal:

`./retris --notermgfx --js /dev/input/js0 --curtain_mac "08:65:F0:XX:XX:XX" --curtain_retry 3000`

