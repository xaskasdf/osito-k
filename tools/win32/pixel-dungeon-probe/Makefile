JAVAC ?= javac
JAR ?= jar
GAME_JAR ?= desktop-1.9.2-steam.jar
BUILD_DIR ?= build
OUTPUT ?= $(BUILD_DIR)/osito-pd-debug.jar

SOURCES := \
	src/com/watabou/pd/desktop/DebugDesktopLauncher.java \
	src/com/watabou/pd/desktop/SteamworksProbe.java \
	src/com/watabou/pd/desktop/TracedPixelDungeon.java \
	src/com/watabou/noosa/NoosaScript.java \
	src/com/watabou/glwrap/Shader.java \
	src/com/badlogic/gdx/backends/lwjgl/TracedLwjglApplication.java \
	src/com/badlogic/gdx/backends/lwjgl/TracedLwjglGraphics.java \
	src/org/lwjgl/opengl/WindowsContextImplementation.java
CLASS_DIR := $(BUILD_DIR)/classes

.PHONY: all clean

all: $(OUTPUT)

$(OUTPUT): $(SOURCES) $(GAME_JAR)
	mkdir -p $(CLASS_DIR)
	$(JAVAC) --release 8 -cp "$(GAME_JAR)" -d $(CLASS_DIR) $(SOURCES)
	$(JAR) cf $(OUTPUT) -C $(CLASS_DIR) .

clean:
	rm -rf $(BUILD_DIR)
