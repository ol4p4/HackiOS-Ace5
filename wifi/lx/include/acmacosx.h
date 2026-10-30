/* ACPICA's acenv.h picks the macOS platform header whenever the compiler
 * target defines __APPLE__ — which clang does for arm64-apple-ios16.5 — and
 * FreeBSD's tree does not carry acmacosx.h. This driver is compiled as
 * FreeBSD kernel code, so use the FreeBSD platform header it ships.
 */
#include <contrib/dev/acpica/include/platform/acfreebsd.h>
