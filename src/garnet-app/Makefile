# Builds GarnetCoreTests, garnetctl, and Garnet.app using Apple Command Line Tools only (no Xcode.app required).
BUILD ?= build
SDK ?= $(shell xcrun --sdk macosx --show-sdk-path)
SWIFTC ?= swiftc
TARGET := x86_64-apple-macosx14.0
SWIFT_FLAGS := -target $(TARGET) -sdk $(SDK) -swift-version 5 -parse-as-library -O
GARNET_EMBED_PAYLOAD ?=

CORE_SRCS := $(sort $(wildcard Sources/GarnetCore/*.swift))
CLI_SRCS := $(sort $(wildcard Sources/GarnetCLI/*.swift))
APP_SRCS := $(sort $(wildcard Sources/GarnetApp/*.swift))
TEST_SRCS := $(sort $(wildcard Tests/GarnetCoreTests/*.swift))

CLI_BIN := $(BUILD)/garnetctl
TEST_BIN := $(BUILD)/GarnetCoreTests
APP_BUNDLE := $(BUILD)/Garnet.app

.PHONY: all cli app test clean

all: cli app

$(BUILD):
	mkdir -p $(BUILD)

cli: $(CLI_BIN)

$(CLI_BIN): $(CORE_SRCS) $(CLI_SRCS) | $(BUILD)
	$(SWIFTC) $(SWIFT_FLAGS) $(CORE_SRCS) $(CLI_SRCS) \
		-framework Foundation -framework CryptoKit -framework IOKit \
		-o $@
	xattr -cr $@ 2>/dev/null || true
	codesign --force --sign - $@

$(TEST_BIN): $(CORE_SRCS) $(TEST_SRCS) | $(BUILD)
	$(SWIFTC) $(SWIFT_FLAGS) $(CORE_SRCS) $(TEST_SRCS) \
		-framework Foundation -framework CryptoKit -framework IOKit \
		-o $@

test: $(TEST_BIN)
	$(if $(findstring /,$(TEST_BIN)),$(TEST_BIN),./$(TEST_BIN))

app: $(APP_BUNDLE)

$(APP_BUNDLE): $(CORE_SRCS) $(APP_SRCS) $(CLI_BIN) Resources/Info.plist | $(BUILD)
	rm -rf $(APP_BUNDLE)
	mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources
	cp Resources/Info.plist $(APP_BUNDLE)/Contents/Info.plist
	$(SWIFTC) $(SWIFT_FLAGS) $(CORE_SRCS) $(APP_SRCS) \
		-framework SwiftUI -framework AppKit -framework Foundation -framework CryptoKit -framework IOKit \
		-o $(APP_BUNDLE)/Contents/MacOS/Garnet
	cp $(CLI_BIN) $(APP_BUNDLE)/Contents/Resources/garnetctl
	chmod 755 $(APP_BUNDLE)/Contents/MacOS/Garnet $(APP_BUNDLE)/Contents/Resources/garnetctl
	if [ -n "$(GARNET_EMBED_PAYLOAD)" ]; then \
		[ -d "$(GARNET_EMBED_PAYLOAD)" ] || { echo "GARNET_EMBED_PAYLOAD not found: $(GARNET_EMBED_PAYLOAD)" >&2; exit 1; }; \
		mkdir -p $(APP_BUNDLE)/Contents/Resources/Payload; \
		cp -R "$(GARNET_EMBED_PAYLOAD)"/. $(APP_BUNDLE)/Contents/Resources/Payload/; \
	fi
	find $(APP_BUNDLE) \( -name '.DS_Store' -o -name '._*' \) -delete
	xattr -cr $(APP_BUNDLE) 2>/dev/null || true
	codesign --force --deep --sign - $(APP_BUNDLE)
	codesign --verify --deep --strict $(APP_BUNDLE)

clean:
	rm -rf $(BUILD)
