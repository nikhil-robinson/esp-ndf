Double-precision math (and erff/erfcf/tgammaf/lgammaf) from musl libc 1.2.5
(git tag v1.2.5, commit 0784374), MIT license: see COPYRIGHT.

Compiled as CALL0 code (the toolchain's prebuilt newlib libm is windowed and
cannot be linked). The double arithmetic underneath is esp-ndf's own
(lib/double.c, lib/double.S). Unchanged except libm.h (esp-ndf shims at the
top). sqrt and all other float functions are esp-ndf's (lib/math.c, lib/mathf.c).
