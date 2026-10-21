# XSPMan

C management code for loading resources for use with [XSP](https://github.com/yosshin4004/x68k_xsp).

At the end of the day this is just maintaining some parallel lists of resources, but does not interact with Human68k, the X68000 hardware, or XSPlib directly. As such all the code here is regular C99 with no dependencies.

# Bundles

Ordinarily XSP data is loaded with a number of files that differ by extension, representing CHR data, layout information, etc. The code here supports "XSP bundles" which are an optional output from [png2xsp](https://github.com/mikejmoffitt/png2xsp).
