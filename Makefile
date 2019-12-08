
VERSION=0.2.0

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

LIBS=

OBJ=main.obj
SRC=main.cxx

all: $(OBJ)
	"$(LD)" $(LDFLAGS) $(OBJ) $(LIBS) /out:$(EXE_BASE_NAME).exe

$(OBJ):
	@echo #define VERSION "$(VERSION)" > config.h
	"$(CXX)" $(CXXFLAGS) /c $(SRC)

clean:
	del /f /q *.obj *.exe *.pdb *.ilk

