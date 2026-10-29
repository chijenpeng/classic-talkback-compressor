# Classic Talkback Compressor release notes

## v0.1.0

First public release.

Classic Talkback Compressor models the SSL CF82B33 talkback (listen mic) compressor card from its two
factory drawings, selectable as **Early** (the CF82B33 sheet) and **1984** (T82033.71 rev 6a).

- **Circuit.** A JFET shunt attenuator in a feedback limiter, followed by the output amplifier that
  sets the card's 290 Hz to 4 kHz band, with its rectifier and storage capacitor. The response changes
  with compression, and the two revisions differ in distortion and release. Switching Circuit
  while playing crosses over in 60 ms.
- **Controls.** Circuit, Input, Bias, Output, Mix and Stereo (Dual / Link / M/S). Bias is the card's bias
  trim from 0.30 to 1.00 (default 0.50) and changes both the threshold and the character.
- **Engine.** A reduced-circuit solver built from the drawn component values, at 4x oversampling,
  44 samples of latency at every sample rate. About 456 ns per frame mono and 916 ns stereo at 48 kHz.
- **Gain-reduction needle** with a VU face and 65 ms ballistics, no numeric readout.
- **Checked** row by row against a simulation of the full card for both circuits. No hardware was
  measured. Input and Output levels, the Bias range and default, the needle's face, Mix and the
  stereo modes are choices of this plug-in; the README says which.
- **Known limits.** The engine misses some overload rows (clip-onset harmonics, a few even harmonics at
  very high drive), shows alias lines up to about −30 dBc at 44.1 kHz when the preamp is driven hard,
  and the 1984 circuit at Bias 1.00 is checked against the engine only. Details are in the README.

Packages:

- macOS Universal: AU and VST3, arm64 + x86_64; ad-hoc signed, not notarized.
- Windows x64: VST3, unsigned.

Identity: `com.c0dypeng.classictalkbackcompressor`, manufacturer `Cdpg`, plug-in code `CtCp`.
