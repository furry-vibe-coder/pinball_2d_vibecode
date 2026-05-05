CXX := g++
EMSDK_EMXX := C:/GameDev/SDL/emsdk/upstream/emscripten/em++.bat
EMXX := $(if $(wildcard $(EMSDK_EMXX)),$(EMSDK_EMXX),em++)
MSYS2_ROOT ?= /c/GameDev/SDL/msys2
UCRT64 ?= $(MSYS2_ROOT)/ucrt64
INCLUDES := -I$(UCRT64)/include -I$(UCRT64)/include/SDL2
LIBPATHS := -L$(UCRT64)/lib
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pedantic $(INCLUDES)
BASE_LIBS := -lmingw32 -lSDL2main -lSDL2 -lSDL2_image -lSDL2_ttf
MIXER_HEADER := $(UCRT64)/include/SDL2/SDL_mixer.h
MIXER_LIB := $(UCRT64)/lib/libSDL2_mixer.dll.a
ifeq ($(wildcard $(MIXER_HEADER) $(MIXER_LIB)),)
MIXER_LIBS :=
else
MIXER_LIBS := -lSDL2_mixer
endif
LDFLAGS := $(LIBPATHS) $(BASE_LIBS) $(MIXER_LIBS)

TARGET := pinball
SRC := main.cpp
OBJ := $(SRC:.cpp=.o)
WEB_DIR := web
WEB_TARGET := $(WEB_DIR)/index.html
WEB_CXXFLAGS := -std=c++17 -O3
WEB_LDFLAGS := -sUSE_SDL=2 -sUSE_SDL_IMAGE=2 -sUSE_SDL_TTF=2 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 -sWASM=1 -sENVIRONMENT=web -sNO_EXIT_RUNTIME=1 -sASYNCIFY=1

.PHONY: all clean run
.PHONY: bundle
.PHONY: web serve-web web-zip

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	sh ./run_pinball.sh

web:
	mkdir -p "$(WEB_DIR)"
	$(EMXX) $(WEB_CXXFLAGS) $(SRC) -o "$(WEB_TARGET)" $(WEB_LDFLAGS)

serve-web: web
	python -m http.server 8000 --directory "$(WEB_DIR)"

web-zip:
	rm -f pinball_itch_html5.zip
	powershell -NoProfile -Command "Compress-Archive -Path 'web\\*' -DestinationPath 'pinball_itch_html5.zip' -Force"

bundle: $(TARGET)
	cp -f "$(UCRT64)/bin/SDL2.dll" .
	cp -f "$(UCRT64)/bin/SDL2_image.dll" .
	cp -f "$(UCRT64)/bin/SDL2_ttf.dll" .
	-test -f "$(UCRT64)/bin/SDL2_mixer.dll" && cp -f "$(UCRT64)/bin/SDL2_mixer.dll" . || true
	cp -f "$(UCRT64)/bin/libstdc++-6.dll" .
	cp -f "$(UCRT64)/bin/libgcc_s_seh-1.dll" .
	cp -f "$(UCRT64)/bin/libwinpthread-1.dll" .
	cp -f "$(UCRT64)/bin/zlib1.dll" .
	cp -f "$(UCRT64)/bin/libpng16-16.dll" .
	cp -f "$(UCRT64)/bin/libfreetype-6.dll" .
	cp -f "$(UCRT64)/bin/libbrotlidec.dll" .
	cp -f "$(UCRT64)/bin/libbrotlicommon.dll" .
	cp -f "$(UCRT64)/bin/libbz2-1.dll" .
	cp -f "$(UCRT64)/bin/libharfbuzz-0.dll" .
	cp -f "$(UCRT64)/bin/libgraphite2.dll" .
	cp -f "$(UCRT64)/bin/libglib-2.0-0.dll" .
	cp -f "$(UCRT64)/bin/libintl-8.dll" .
	cp -f "$(UCRT64)/bin/libiconv-2.dll" .
	cp -f "$(UCRT64)/bin/libpcre2-8-0.dll" .
	cp -f "$(UCRT64)/bin/libjpeg-8.dll" .
	cp -f "$(UCRT64)/bin/libwebp-7.dll" .
	cp -f "$(UCRT64)/bin/libtiff-6.dll" .
	cp -f "$(UCRT64)/bin/liblzma-5.dll" .
	cp -f "$(UCRT64)/bin/libzstd.dll" .

clean:
	rm -f $(OBJ) $(TARGET).exe $(TARGET)
	rm -rf "$(WEB_DIR)"
