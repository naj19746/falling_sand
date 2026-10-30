# sandbg keymap: its own raw HID receiver, no VIA.
RAW_ENABLE = yes
VIA_ENABLE = no
RGB_MATRIX_CUSTOM_USER = yes

# Keychron's common raw HID file defines raw_hid_receive when VIA is off;
# sandbg's keymap.c defines it instead. SRC := would make SRC simply
# expanded, and QMK appends files to it later that name variables not set
# yet, so filter its unexpanded text and keep it recursive.
$(eval SRC = $(filter-out %/keychron_raw_hid.c,$(value SRC)))
SRC += sandbg_raw_hid.c
