// This file is part of SmallBASIC
//
// Copyright(C) 2001-2012 Chris Warren-Smith.
//
// This program is distributed under the terms of the GPL v2.0 or later
// Download the GNU Public License (GPL) from www.gnu.org
//

#include <ma.h>
#include <MAUI/Engine.h>
#include <MAUI/Font.h>

#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "MAHeaders.h"

#include "platform/mosync/controller.h"
#include "platform/mosync/utils.h"

#define LONG_PRESS_TIME 3000

Controller::Controller() :
  Environment(),
  output(0),
  runMode(init_state),
  lastEventTime(0),
  eventsPerTick(0),
  penMode(PEN_OFF),
  penDownX(-1),
  penDownY(-1),
  penDownTime(0) {
  logEntered();
}

bool Controller::construct() {
  MAExtent screenSize = maGetScrSize();
  output = new AnsiWidget(this, EXTENT_X(screenSize), EXTENT_Y(screenSize));
  output->construct();

  // install the default font
  MAUI::Engine& engine = MAUI::Engine::getSingleton();
  engine.setDefaultFont(new MAUI::Font(RES_FONT));

  runMode = init_state;
  opt_ide = IDE_NONE;
  opt_graphics = true;
  opt_pref_bpp = 0;
  opt_nosave = true;
  opt_interactive = true;
  opt_verbose = false;
  opt_quiet = true;
  opt_command[0] = 0;
  opt_usevmt = 0;
  os_graphics = 1;

  return true;
}

Controller::~Controller() {
  delete output;
}

const char *Controller::getLoadPath() {
  return !loadPath.size() ? NULL : loadPath.c_str();
}

int Controller::getPen(int code) {
  int result = 0;

  if (isExit()) {
    ui_reset();
    brun_break();
  } else {
    if (penMode == PEN_OFF) {
      processEvents(0, -1);
    }

    switch (code) {
    case 0:
      // UNTIL PEN(0) - wait until move click or move
      processEvents(1, -1);    // fallthru to re-test

    case 3:                    // returns true if the pen is down (and save curpos)
      processEvents(0, -1);
      if (penDownX != -1 && penDownY != -1) {
        result = 1;
      }
      break;

    case 1:                      // last pen-down x
      result = penDownX;
      break;

    case 2:                      // last pen-down y
      result = penDownY;
      break;

    case 4:                      // cur pen-down x
    case 10:
      processEvents(0, -1);
      result = penDownX;
      break;

    case 5:                      // cur pen-down y
    case 11:
      processEvents(0, -1);
      result = penDownY;
      break;
    }
  }
  return result;
}

// whether a GUI is active which may yield a load path
bool Controller::hasUI() {
  return output->hasUI();
}

// runtime system event processor
int Controller::handleEvents(int waitFlag) {
  if (!waitFlag) {
    // pause when we have been called too frequently
    int now = maGetMilliSecondCount();
    if (now - lastEventTime <= EVT_CHECK_EVERY) {
      eventsPerTick += (now - lastEventTime);
      if (eventsPerTick >= EVT_MAX_BURN_TIME) {
        eventsPerTick = 0;
        waitFlag = 2;
      }
    }
    lastEventTime = now;
  }

  switch (waitFlag) {
  case 1:
    // wait for an event
    processEvents(-1, -1);
    break;
  case 2:
    // pause
    processEvents(EVT_PAUSE_TIME, -1);
    break;
  default:
    // pump messages without pausing
    processEvents(0, -1);
    break;
  }

  output->flush(true);
  return isExit() ? -2 : 0;
}

// process events while in modal state
void Controller::modalLoop() {
  runMode = modal_state;
  while (runMode == modal_state) {
    processEvents(-1, -1);
  }
}

// pause for the given number of milliseconds
void Controller::pause(int ms) {
  if (runMode == run_state) {
    int msWait = ms / 2;
    int msStart = maGetMilliSecondCount();
    runMode = modal_state;
    while (runMode == modal_state) {
      if (maGetMilliSecondCount() - msStart >= ms) {
        runMode = run_state;
        break;
      }
      processEvents(msWait, -1);
    }
  } else {
    MAEvent event;
    int msStart = maGetMilliSecondCount();
    while (maGetMilliSecondCount() - msStart < ms) {
      if (!maGetEvent(&event)) {
        maWait(10);
      }
    }
  }
}

// process events on the system event queue
MAEvent Controller::processEvents(int ms, int untilType) {
  MAEvent event;
  MAExtent screenSize;

  if (penDownTime != 0) {
    int now = maGetMilliSecondCount();
    if ((now - penDownTime) > LONG_PRESS_TIME) {
      penDownTime = now;
    }
  }

  while (!isExit() && maGetEvent(&event)) {
    if (isModal()) {
      // process events for any active GUI
      fireEvent(event);
    }

    switch (event.type) {
    case EVENT_TYPE_SCREEN_CHANGED:
      screenSize = maGetScrSize();
      output->resize(EXTENT_X(screenSize), EXTENT_Y(screenSize));
      os_graf_mx = output->getWidth();
      os_graf_my = output->getHeight();
      handleKey(SB_PKEY_SIZE_CHG);
      break;
    case EVENT_TYPE_POINTER_PRESSED:
      penDownTime = maGetMilliSecondCount();
      penDownX = event.point.x;
      penDownY = event.point.y;
      handleKey(SB_KEY_MK_PUSH);
      output->pointerTouchEvent(event);
      break;
    case EVENT_TYPE_POINTER_DRAGGED:
      if (event.point.y < output->getHeight()) {
        output->pointerMoveEvent(event);
      }
      break;
    case EVENT_TYPE_POINTER_RELEASED:
      if (event.point.y < output->getHeight()) {
        penDownTime = 0;
        penDownX = penDownY = -1;
        handleKey(SB_KEY_MK_RELEASE);
        output->pointerReleaseEvent(event);
      }
      break;
    case EVENT_TYPE_CLOSE:
      runMode = exit_state;
      break;
    case EVENT_TYPE_KEY_PRESSED:
      handleKey(event.key);
      break;
    }
    if (untilType != -1 && untilType == event.type) {
      // found target event
      break;
    }
  }

  if (runMode == exit_state) {
    // terminate the running program
    ui_reset();
    brun_break();
  }
  else {
    // pump messages into the engine
    runIdleListeners();
    if (ms != 0) {
      maWait(ms);
    }
  }

  return event;
}

char *Controller::readSource(const char *fileName) {
  char *buffer = NULL;
  
  if (strcasecmp(MAIN_BAS_RES, fileName) == 0) {
    // load as resource
    int len = maGetDataSize(MAIN_BAS);
    buffer = (char *)mem_alloc(len + 1);
    maReadData(MAIN_BAS, buffer, 0, len);
    buffer[len] = '\0';
  } else if (strstr(fileName, "://")) {
    buffer = readConnection(fileName);
  } else {
    // load from file system
    MAHandle handle = maFileOpen(fileName, MA_ACCESS_READ);
    if (maFileExists(handle)) {
      int len = maFileSize(handle);
      buffer = (char *)mem_alloc(len);
      maFileRead(handle, buffer, len);
    }
    maFileClose(handle);
  }

  if (buffer == NULL) {
    buffer = (char *)mem_alloc(strlen(ERROR_BAS) + 1);
    strcpy(buffer, ERROR_BAS);
  }

  return buffer;
}

// commence runtime state
void Controller::setRunning() {
  logEntered();

  dev_fgcolor = -DEFAULT_COLOR;
  dev_bgcolor = 0;
  os_graf_mx = output->getWidth();
  os_graf_my = output->getHeight();

  os_ver = 1;
  os_color = 1;
  os_color_depth = 16;
  setsysvar_str(SYSVAR_OSNAME, "MoSync");

  osd_cls();
  dev_clrkb();
  ui_reset();

  loadPath.clear();
  runMode = run_state;
}

// handler for hyperlink click actions
void Controller::buttonClicked(const char *url) {
  loadPath.clear();
  int len = strlen(url);
  if (len == 1) {
    handleKey(url[0]);
  } else {
    loadPath.append(url, strlen(url));
  }
}

// pass the event into the mosync framework
void Controller::fireEvent(MAEvent &event) {
  switch (event.type) {
  case EVENT_TYPE_CLOSE:
    fireCloseEvent();
    break;
  case EVENT_TYPE_FOCUS_GAINED:
    fireFocusGainedEvent();
    break;
  case EVENT_TYPE_FOCUS_LOST:
    fireFocusLostEvent();
    break;
  case EVENT_TYPE_KEY_PRESSED:
    fireKeyPressEvent(event.key, event.nativeKey);
    break;
  case EVENT_TYPE_KEY_RELEASED:
    fireKeyReleaseEvent(event.key, event.nativeKey);
    break;
  case EVENT_TYPE_CHAR:
    fireCharEvent(event.character);
    break;
  case EVENT_TYPE_POINTER_PRESSED:
    if (event.touchId == 0) {
      firePointerPressEvent(event.point);
    }
    fireMultitouchPressEvent(event.point, event.touchId);
    break;
  case EVENT_TYPE_POINTER_DRAGGED:
    if (event.touchId == 0) {
      firePointerMoveEvent(event.point);
    }
    fireMultitouchMoveEvent(event.point, event.touchId);
    break;
  case EVENT_TYPE_POINTER_RELEASED:
    if (event.touchId == 0) {
      firePointerReleaseEvent(event.point);
    }
    fireMultitouchReleaseEvent(event.point, event.touchId);
    break;
  case EVENT_TYPE_CONN:
    fireConnEvent(event.conn);
    break;
  case EVENT_TYPE_BT:
    fireBluetoothEvent(event.state);
    break;
  case EVENT_TYPE_TEXTBOX:
    fireTextBoxListeners(event.textboxResult, event.textboxLength);
    break;
  case EVENT_TYPE_SENSOR:
    fireSensorListeners(event.sensor);
    break;
  default:
    fireCustomEventListeners(event);
    break;
  }
}

// pass the key into the smallbasic keyboard handler
void Controller::handleKey(int key) {
  switch (key) {
  case MAK_FIRE:
  case MAK_5:
    break;
  case MAK_SOFTRIGHT:
  case MAK_BACK:
    runMode = exit_state;
    break;
  }

  if (isRunning()) {
    switch (key) {
    case MAK_TAB:
      dev_pushkey(SB_KEY_TAB);
      break;
    case MAK_HOME:
      dev_pushkey(SB_KEY_KP_HOME);
      break;
    case MAK_END:
      dev_pushkey(SB_KEY_END);
      break;
    case MAK_INSERT:
      dev_pushkey(SB_KEY_INSERT);
      break;
    case MAK_MENU:
      dev_pushkey(SB_KEY_MENU);
      break;
    case MAK_KP_MULTIPLY:
      dev_pushkey(SB_KEY_KP_MUL);
      break;
    case MAK_KP_PLUS:
      dev_pushkey(SB_KEY_KP_PLUS);
      break;
    case MAK_KP_MINUS:
      dev_pushkey(SB_KEY_KP_MINUS);
      break;
    case MAK_SLASH:
      dev_pushkey(SB_KEY_KP_DIV);
      break;
    case MAK_PAGEUP:
      dev_pushkey(SB_KEY_PGUP);
      break;
    case MAK_PAGEDOWN:
      dev_pushkey(SB_KEY_PGDN);
      break;
    case MAK_UP:
      dev_pushkey(SB_KEY_UP);
      break;
    case MAK_DOWN:
      dev_pushkey(SB_KEY_DN);
      break;
    case MAK_LEFT:
      dev_pushkey(SB_KEY_LEFT);
      break;
    case MAK_RIGHT:
      dev_pushkey(SB_KEY_RIGHT);
      break;
    case MAK_BACKSPACE:
    case MAK_DELETE:
      dev_pushkey(SB_KEY_BACKSPACE);
      break;
    default:
      dev_pushkey(key);
      break;
    }
  }
}

// returns the contents of the given url
char *Controller::readConnection(const char *url) {
  char *result = NULL;

  MAHandle conn = maConnect(url);
  if (conn > 0) {
    runMode = conn_state;
    output->print("Connecting to ");
    output->print(url);
    bool connected = false;
    char buffer[1024];
    int length = 0;
    int size = 0;
    MAEvent event;

    // pause until connected
    while (runMode == conn_state) {
      event = processEvents(50, EVENT_TYPE_CONN);
      if (event.type == EVENT_TYPE_CONN) {
        switch (event.conn.opType) {
        case CONNOP_CONNECT:
          // connection established
          if (!connected) {
            connected = (event.conn.result > 0);
            if (connected) {
              maConnRead(conn, buffer, sizeof(buffer) - 1);
            } else {
              runMode = init_state;
            }
          }
          break;
        case CONNOP_READ:
          // connRead completed
          if (event.conn.result > 0) {
            size = event.conn.result;
            buffer[size] = 0;
            if (result == NULL) {
              result = (char *)tmp_alloc(size + 1);
              strncpy(result, buffer, size);
              length = size;
            } else {
              result = (char *)tmp_realloc(result, length + size + 1);
              strncpy(result + length, buffer, size);
              length += size;
            }
            result[length] = 0;
            // try to read more data
            maConnRead(conn, buffer, sizeof(buffer) - 1);
          } else {
            // no more data
            runMode = init_state;
          }
          break;
        }
      }
    }
    maConnClose(conn);
  }
  return result;
}
