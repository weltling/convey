
# The version defaults to `git describe`; override with `nmake VERSION=x`.
# Falls back to the literal below when git is unavailable (e.g. tarballs).
VERSION=0.4.1
!IF [git describe --tags --always --dirty 2> NUL > convey_gitver.tmp] == 0
!	IF [for /f "usebackq delims=" %v in (convey_gitver.tmp) do @echo VERSION=%v> convey_gitver.mk] == 0
!		INCLUDE convey_gitver.mk
!	ENDIF
!ENDIF


!if "$(CXX)" == ""
CXX=cl.exe
!endif

!if "$(LD)" == ""
LD=link.exe
!endif

CXXFLAGS=/nologo /utf-8 /W3 /Zc:__cplusplus /Zc:wchar_t /EHsc /Zi
LDFLAGS=/nologo

EXE_BASE_NAME=convey

!if "$(DEBUG)" == ""
CXXFLAGS=$(CXXFLAGS) /MT /Ox /Fd:$(EXE_BASE_NAME).pdb
LDFLAGS=$(LDFLAGS) 
!else
CXXFLAGS=$(CXXFLAGS) /MTd /Od /DEBUG /D_DEBUG
LDFLAGS=$(LDFLAGS) /DEBUG
!endif

LIBS=user32.lib ws2_32.lib

OBJ=main.obj
SRC=main.cxx

all: $(OBJ)
	"$(LD)" $(LDFLAGS) $(OBJ) $(LIBS) /out:$(EXE_BASE_NAME).exe

$(OBJ):
	@echo #define VERSION "$(VERSION)" > config.h
	"$(CXX)" $(CXXFLAGS) /c $(SRC)

clean:
	del /f /q *.obj *.exe *.pdb *.ilk convey_gitver.tmp convey_gitver.mk

test: all
	powershell -NoProfile -ExecutionPolicy Bypass -File test\tcp.ps1 -Convey $(EXE_BASE_NAME).exe

unit:
	@echo #define VERSION "$(VERSION)" > config.h
	"$(CXX)" $(CXXFLAGS) /Fe:convey_unit.exe test\unit.cxx $(LIBS)
	convey_unit.exe

