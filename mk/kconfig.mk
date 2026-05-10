# mk/kconfig.mk - Kconfiglib integration for Mazu
#
# Provides: make config, make defconfig, make oldconfig, make savedefconfig
# Generates: .config (Make-includable) and $(HEADER_CONFIG) (C header)

ifndef _MK_KCONFIG_INCLUDED
_MK_KCONFIG_INCLUDED := 1

KCONFIG_DIR    := tools/kconfig
KCONFIG_REPO   := https://github.com/sysprog21/Kconfiglib.git
KCONFIG_SCHEMA := configs/Kconfig

# Auto-clone Kconfiglib on first use
$(KCONFIG_DIR)/menuconfig.py:
	@echo "  CLONE   Kconfiglib -> $(KCONFIG_DIR)"
	@git clone --depth 1 $(KCONFIG_REPO) $(KCONFIG_DIR)

# Interactive menuconfig TUI
config: $(KCONFIG_DIR)/menuconfig.py
	@KCONFIG_CONFIG=.config python3 $(KCONFIG_DIR)/menuconfig.py $(KCONFIG_SCHEMA)
	@rm -f $(HEADER_CONFIG)

# Apply a defconfig (default: configs/defconfig). Optional CONFIG_FRAGMENTS
# are merged in order to avoid cloning near-identical defconfigs for CI or
# sanitizer-only variants.
DEFCONFIG ?= configs/defconfig
CONFIG_FRAGMENTS ?=
defconfig: $(KCONFIG_DIR)/menuconfig.py
	@python3 ./scripts/apply_defconfig.py \
		--kconfig $(KCONFIG_SCHEMA) \
		--out .config \
		$(DEFCONFIG) $(CONFIG_FRAGMENTS)
	@rm -f $(HEADER_CONFIG)

# Update .config for new/changed Kconfig symbols
oldconfig: $(KCONFIG_DIR)/menuconfig.py
	@KCONFIG_CONFIG=.config python3 $(KCONFIG_DIR)/oldconfig.py $(KCONFIG_SCHEMA)
	@rm -f $(HEADER_CONFIG)

# Save minimal config
savedefconfig: $(KCONFIG_DIR)/menuconfig.py
	@KCONFIG_CONFIG=.config python3 $(KCONFIG_DIR)/savedefconfig.py \
		--kconfig $(KCONFIG_SCHEMA) --out configs/defconfig
	@echo "  SAVE    configs/defconfig"

endif # _MK_KCONFIG_INCLUDED
