# FS-UAE host-JIT checkpoint

Current qualified results and cross-mode byte-identical frame validation are
in summary.json, qualified-qrx-off, qualified-qrx-on and control-no-host-jit.
Native-cpuonly-final contains the passing suite for the final selected profile.
Exploratory FPU-JIT runs have a two-pixel difference from the control; they are
not the selected configuration. Rejected melt trials are excluded from claims.
The build uses upstream4ae7ddaec50b567ed80d71ffbff067cb58e945a3 plus the patch
in tools/qrx86/fsuae. Full build/source stays outside the project checkout.
No physical hardware measurement is claimed. New work targets25FPS on FS-UAE.
