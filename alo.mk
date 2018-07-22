######################################
#
# alo
#
######################################

# ALO_VERSION = master
# ALO_SITE = $(call github, devcurmudgeon,alo, $(ALO_VERSION))

# or for local development ...
ALO_SITE_METHOD = local
ALO_SITE = /tmp/moddevices/alo/source

ALO_VERSION = 0.1

# dependencies (list of other buildroot packages, separated by space)
ALO_DEPENDENCIES =

# LV2 bundles that this package generates (space separated list)
ALO_BUNDLES = alo.lv2

# call make with the current arguments and path. "$(@D)" is the build directory.
ALO_TARGET_MAKE = $(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D)

# build command
define ALO_BUILD_CMDS
	$(ALO_TARGET_MAKE)
endef

# install command
define ALO_INSTALL_TARGET_CMDS
	$(ALO_TARGET_MAKE) install DESTDIR=$(TARGET_DIR)
endef

# import everything else from the buildroot generic package
$(eval $(generic-package))
