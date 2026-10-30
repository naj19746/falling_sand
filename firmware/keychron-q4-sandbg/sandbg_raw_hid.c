/* The Q4's common build includes keychron_raw_hid.c, which defines
 * raw_hid_receive itself when VIA is disabled. sandbg's keymap brings its
 * own receiver, so rules.mk drops that file. The common code still calls
 * kc_raw_hid_send() from factory_test.c, so provide it here.
 */
#include "quantum.h"
#include "raw_hid.h"

void kc_raw_hid_send(uint8_t src, uint8_t *data, uint8_t len);

void kc_raw_hid_send(uint8_t src, uint8_t *data, uint8_t len)
{
    (void)src;
    raw_hid_send(data, len);
}
