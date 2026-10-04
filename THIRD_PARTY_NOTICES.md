# Third-Party Notices

The repository's root MIT license applies to original code authored for this
project. The following data retains its upstream copyright and license.

## Mesa/Freedreno A8xx performance-counter data

Included or derived files:

- `data/a8xx_perfcntrs.xml`
- `data/freedreno_copyright.xml`
- `include/a8xx_perf_table.inc`, generated from `a8xx_perfcntrs.xml`

Upstream repository:

- <https://gitlab.freedesktop.org/mesa/mesa.git>
- Commit: `4027f06090eec398fab0d4facaa431c4104ec367`

Upstream source paths:

- `src/freedreno/registers/adreno/a8xx_perfcntrs.xml`
- `src/freedreno/registers/freedreno_copyright.xml`

The upstream copyright metadata names Rob Clark as the initial author and Ilia
Mirkin for many A3xx/A4xx contributions, with year 2013. The original metadata
is preserved verbatim in `data/freedreno_copyright.xml`.

Upstream license text:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice (including the next
> paragraph) shall be included in all copies or substantial portions of the
> Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
> OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
> FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
> IN THE SOFTWARE.

## KGSL userspace ABI reference

The minimal perfcounter ioctl numbers and structure layouts used by the C
sources correspond to the public Android KGSL userspace interface documented in
`include/uapi/linux/msm_kgsl.h`:

<https://android.googlesource.com/kernel/msm/+/android-7.1.0_r0.2/include/uapi/linux/msm_kgsl.h>

The full upstream header is not redistributed in this repository. Counter group
mapping beyond the stable ioctl layout is device- and kernel-dependent.
