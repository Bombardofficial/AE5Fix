# AE5 Fix

A key that puts a stuck Sound Blaster AE-5 or AE-5 Plus back. When the sound goes thin, press it. The window does not have to be open, and you do not have to stop what you are doing.

## Get it

1. Download [AE5Fix-1.0.0.zip](https://github.com/Bombardofficial/AE5Fix/releases/latest).
2. Unzip it. Leave `AE5Fix.exe` and `sbz-switch.exe` in that folder.
3. Run `AE5Fix.exe`.
4. Set **Listening on** to Headphones or Speakers, whichever you use.

**Start with Windows** keeps the key after a restart. Closing the window leaves it by the clock. Right-click the icon and choose **Exit** to quit.

Sound Blaster Command has to be installed, and Windows has to be playing through the AE-5.

## The key

Press the shortcut when the sound is wrong. It switches away from your output and straight back. That is the same reset as the headphones and speakers control in Sound Blaster Command.

It starts on F10. **Change** sets another key. **Fix now** is the same reset from the window. **Listening on** is the output it returns to.

It will not press itself, and it will not stop this happening again. If the card is missing in Windows, this cannot bring it back.

The switch is [sbz-switch](https://github.com/mdonoughe/sbz-switch), included in the zip with its license. AE5 Fix is MIT. See `LICENSE`.

## Build it yourself

Open `AE5Fix.slnx` in Visual Studio, build Release x64, and put `sbz-switch.exe` next to the exe you built.
