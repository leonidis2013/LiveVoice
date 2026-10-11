# Third-party software

LiveVoice (Copyright (C) 2026 Jerod Ulicki, AGPL-3.0-only) is built with the following. All DSP, analysis and UI code in `Source/` is original to this project.

| Component | Use | License |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 8.0.10 | Plug-in framework, GUI, FFT, audio utilities (fetched at build time, not stored in this repo) | AGPLv3 (JUCE's open-source option) |
| Steinberg VST3 SDK | VST3 plug-in interface, bundled inside JUCE | Under the terms that ship with JUCE 8.0.10 (see the licence files inside the JUCE download) |
| Inno Setup | Builds the Windows installer (build tool only, nothing is shipped from it) | Inno Setup License |
| `pkgbuild` / `productbuild` | Builds the macOS installer (Apple tools, build only) | Apple |

Algorithms are implemented from public descriptions: topology-preserving-transform state-variable filters (zero-delay feedback), Butterworth high/low-pass cascades, state-variable bell and shelf filters, and Chebyshev polynomials for harmonic generation.

VST is a trademark of Steinberg Media Technologies GmbH.
