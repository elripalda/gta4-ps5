# GTA IV for PS5 - RecompTrophy's payloads (GPL-3.0, by lamarking, used with
# permission: https://github.com/cptlamarca/RecompTrophy), packaged in the app as
# payloads/recomptrophy-relay.elf and payloads/recomptrophy-watcher.elf
# (ps5/tools/ps5_package.sh builds them). Built as RecompTrophy's tools/build.sh does.
#   make PS5_PAYLOAD_SDK=<sdk>
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

RELAY ?= recomptrophy-relay.elf
WATCHER ?= recomptrophy-watcher.elf

all: $(RELAY) $(WATCHER)

$(RELAY): relay.c trophy_progress.c
	$(CC) -O2 -o $@ relay.c

$(WATCHER): watcher.c trophy_progress.c
	$(CC) -O2 -o $@ watcher.c -lSceNotification

clean:
	rm -f $(RELAY) $(WATCHER)
