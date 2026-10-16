# FSMac build
#
#   make            build everything (engine, NFS server, tools, app)
#   make engine     C engine + worker + tools only
#   make test       run the automated test suite
#   make app        build and sign FSMac.app (Xcode or the Command Line Tools)
#   make dist       build the distributable disk image
#   make clean      remove build products (keeps vendor builds)
#
ROOT        := $(abspath .)
BUILD       := $(ROOT)/build
OBJ         := $(BUILD)/obj
BIN         := $(BUILD)/bin

NTFS_SRC    := $(ROOT)/vendor/ntfs-3g
NTFS_BUILD  := $(BUILD)/vendor/ntfs-3g
E2FS_SRC    := $(ROOT)/vendor/e2fsprogs
E2FS_BUILD  := $(BUILD)/vendor/e2fsprogs

ARCH        ?= $(shell uname -m)
MINOS       ?= 14.0
CC          := clang
OPT         ?= -O2
WARN        := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
               -Wno-missing-field-initializers
CFLAGS      := $(OPT) -g -arch $(ARCH) -mmacosx-version-min=$(MINOS) -std=gnu11 \
               $(WARN) $(EXTRA_CFLAGS)
LDFLAGS     := -arch $(ARCH) -mmacosx-version-min=$(MINOS)

ENGINE_INC  := -I$(ROOT)/engine/include -I$(ROOT)/engine
NTFS_INC    := -DHAVE_CONFIG_H -I$(NTFS_BUILD) -I$(NTFS_SRC)/include/ntfs-3g
E2FS_INC    := -I$(E2FS_BUILD)/lib -I$(E2FS_SRC)/lib

LIBNTFS     := $(NTFS_BUILD)/libntfs-3g/.libs/libntfs-3g.a
E2FS_LIBS   := $(E2FS_BUILD)/lib/libext2fs.a $(E2FS_BUILD)/lib/libe2p.a \
               $(E2FS_BUILD)/lib/libblkid.a $(E2FS_BUILD)/lib/libuuid.a \
               $(E2FS_BUILD)/lib/libcom_err.a
E2FS_JOURNAL:= $(E2FS_BUILD)/misc/journal.o $(E2FS_BUILD)/misc/recovery.o \
               $(E2FS_BUILD)/misc/revoke.o
VENDOR_STAMP:= $(LIBNTFS) $(E2FS_BUILD)/lib/libext2fs.a

ENGINE_LIB  := $(BUILD)/lib/libfsmacengine.a
ENGINE_OBJS := $(OBJ)/engine/blockdev.o $(OBJ)/engine/fsc.o \
               $(OBJ)/engine/ntfs.o $(OBJ)/engine/ext4.o
ENGINE_LINK := $(ENGINE_LIB) $(LIBNTFS) $(E2FS_JOURNAL) $(E2FS_LIBS)

NFS_OBJS    := $(OBJ)/nfs/xdr.o $(OBJ)/nfs/rpc.o $(OBJ)/nfs/nfs3.o \
               $(OBJ)/nfs/mount3.o $(OBJ)/nfs/server.o $(OBJ)/nfs/appledouble.o

TOOLS       := $(BIN)/fsmac-shell $(BIN)/fsmac-worker $(BIN)/fsmac-probe \
               $(BIN)/fsmac_ext.util

.PHONY: all engine vendor test app dist clean distclean shell

all: engine app

shell: $(BIN)/fsmac-shell $(BIN)/fsmac-probe

engine: $(TOOLS) vendor-tools

vendor: $(VENDOR_STAMP)

$(VENDOR_STAMP):
	$(ROOT)/scripts/build-vendor.sh

# Upstream command line tools shipped inside the app bundle.
VENDOR_TOOLS := $(BIN)/mkntfs $(BIN)/ntfsfix $(BIN)/ntfslabel \
                $(BIN)/mke2fs $(BIN)/e2fsck $(BIN)/tune2fs
.PHONY: vendor-tools
vendor-tools: $(VENDOR_TOOLS)
$(BIN)/mkntfs $(BIN)/ntfsfix $(BIN)/ntfslabel: $(VENDOR_STAMP)
	@mkdir -p $(BIN)
	cp $(NTFS_BUILD)/ntfsprogs/$(notdir $@) $@
$(BIN)/mke2fs $(BIN)/tune2fs: $(VENDOR_STAMP)
	@mkdir -p $(BIN)
	cp $(E2FS_BUILD)/misc/$(notdir $@) $@
$(BIN)/e2fsck: $(VENDOR_STAMP)
	@mkdir -p $(BIN)
	cp $(E2FS_BUILD)/e2fsck/e2fsck $@

# --- engine ------------------------------------------------------------------
$(OBJ)/engine/blockdev.o $(OBJ)/engine/fsc.o: $(OBJ)/engine/%.o: engine/%.c engine/*.h engine/include/fsmac/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ENGINE_INC) -c $< -o $@

$(OBJ)/engine/ntfs.o: engine/ntfs.c engine/*.h engine/include/fsmac/*.h $(VENDOR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ENGINE_INC) $(NTFS_INC) -c $< -o $@

$(OBJ)/engine/ext4.o: engine/ext4.c engine/*.h engine/include/fsmac/*.h $(VENDOR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ENGINE_INC) $(E2FS_INC) -Wno-gnu-statement-expression -c $< -o $@

$(ENGINE_LIB): $(ENGINE_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

# --- NFS server --------------------------------------------------------------
$(OBJ)/nfs/%.o: nfs/%.c nfs/*.h engine/include/fsmac/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ENGINE_INC) -Infs -c $< -o $@

# --- programs ----------------------------------------------------------------
$(OBJ)/tools/%.o: tools/%.c engine/include/fsmac/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ENGINE_INC) -Infs -c $< -o $@

$(OBJ)/worker/%.o: worker/%.c engine/include/fsmac/*.h nfs/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ENGINE_INC) -Infs -c $< -o $@

$(BIN)/fsmac-shell: $(OBJ)/tools/fsmac-shell.o $(ENGINE_LIB) $(VENDOR_STAMP)
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $< $(ENGINE_LINK)

$(BIN)/fsmac-probe: $(OBJ)/tools/fsmac-probe.o $(ENGINE_LIB) $(VENDOR_STAMP)
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $< $(ENGINE_LINK)

$(BIN)/fsmac-worker: $(OBJ)/worker/fsmac-worker.o $(NFS_OBJS) $(ENGINE_LIB) $(VENDOR_STAMP)
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $< $(NFS_OBJS) $(ENGINE_LINK)

# Disk Arbitration probe tool (self-contained, no engine).
$(BIN)/fsmac_ext.util: tools/fsmac-fsutil.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# --- tests -------------------------------------------------------------------
test: engine
	$(ROOT)/tests/run-all.sh

# --- app ---------------------------------------------------------------------
app: engine
	$(ROOT)/scripts/build-app.sh

dist: app
	$(ROOT)/scripts/make-dmg.sh

clean:
	rm -rf $(OBJ) $(BIN) $(BUILD)/lib $(BUILD)/app $(BUILD)/dist $(BUILD)/FSMac.app

distclean: clean
	rm -rf $(BUILD)
