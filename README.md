BOM:
<img width="1816" height="831" alt="image" src="https://github.com/user-attachments/assets/da8818e4-20e5-4acb-8892-d772bf5508f3" />
Essentially, this allows for the control of a Sallen Key LPF. The LPF utilizes optocouplers to adjust the cutoff frequency, different optocouplers need different PWM values to match resistances. 
So if you plan on using this you would need to test PWM values with your optocouplers so they match whichever frequencies you want to cutoff.

Also implements a master knife switch mute, and SW mute. output0 should be the speaker, output 1 can be whatever you wish.

Assumes you are using VScode with the PlatformIO extension, alongside a STM32 F303RE connected through a COM-enabled port for terminal messages to be transmit to/from the device.
