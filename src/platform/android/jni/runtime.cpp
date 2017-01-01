// This file is part of SmallBASIC
//
// Copyright(C) 2001-2016 Chris Warren-Smith.
//
// This program is distributed under the terms of the GPL v2.0 or later
// Download the GNU Public License (GPL) from www.gnu.org
//

#include "config.h"
#include <android/native_window.h>
#include <android/keycodes.h>
#include <jni.h>
#include <errno.h>

#include "platform/android/jni/runtime.h"
#include "lib/maapi.h"
#include "ui/utils.h"
#include "languages/messages.en.h"
#include "common/sbapp.h"
#include "common/sys.h"
#include "common/smbas.h"
#include "common/osd.h"
#include "common/device.h"
#include "common/fs_socket_client.h"
#include "common/keymap.h"

#define WAIT_INTERVAL 10
#define MAIN_BAS "__main_bas__"
#define CONFIG_FILE "/settings.txt"
#define PATH_KEY "path"
#define FONT_SCALE_KEY "fontScale2"
#define FONT_ID_KEY "fontId"
#define SERVER_SOCKET_KEY "serverSocket"
#define SERVER_TOKEN_KEY "serverToken"
#define MUTE_AUDIO_KEY "muteAudio"
#define OPT_IDE_KEY "optIde"
#define GBOARD_KEY_QUESTION 274

Runtime *runtime;

MAEvent *getMotionEvent(int type, AInputEvent *event) {
  MAEvent *result = new MAEvent();
  result->type = type;
  result->point.x = AMotionEvent_getX(event, 0);
  result->point.y = AMotionEvent_getY(event, 0);
  return result;
}

int32_t handleInput(android_app *app, AInputEvent *event) {
  int32_t result = 0;
  if (runtime->isActive()) {
    MAEvent *maEvent = NULL;
    switch (AInputEvent_getType(event)) {
    case AINPUT_EVENT_TYPE_MOTION:
      switch (AKeyEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) {
      case AMOTION_EVENT_ACTION_DOWN:
        maEvent = getMotionEvent(EVENT_TYPE_POINTER_PRESSED, event);
        break;
      case AMOTION_EVENT_ACTION_MOVE:
        maEvent = getMotionEvent(EVENT_TYPE_POINTER_DRAGGED, event);
        break;
      case AMOTION_EVENT_ACTION_UP:
        maEvent = getMotionEvent(EVENT_TYPE_POINTER_RELEASED, event);
        break;
      }
      break;
    case AINPUT_EVENT_TYPE_KEY:
      if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN) {
        maEvent = new MAEvent();
        maEvent->type = EVENT_TYPE_KEY_PRESSED;
        maEvent->nativeKey = AKeyEvent_getKeyCode(event);
        maEvent->key = AKeyEvent_getMetaState(event);
      }
      break;
    }
    if (maEvent != NULL) {
      result = 1;
      runtime->pushEvent(maEvent);
    }
  }
  return result;
}

void handleCommand(android_app *app, int32_t cmd) {
  trace("handleCommand = %d", cmd);
  switch (cmd) {
  case APP_CMD_INIT_WINDOW:
    // thread is ready to start or resume
    if (runtime->isInitial()) {
      runtime->construct();
    }
    break;
  case APP_CMD_GAINED_FOCUS:
    trace("gainedFocus");
    runtime->setFocus(true);
    runtime->redraw();
    break;
  case APP_CMD_LOST_FOCUS:
    trace("lostFocus");
    runtime->setFocus(false);
    break;
  }
}

// see http://stackoverflow.com/questions/15913080
static void process_input(android_app *app, android_poll_source *source) {
  AInputEvent* event = NULL;
  while (AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY &&
        AKeyEvent_getKeyCode(event) == AKEYCODE_BACK) {
      // prevent AInputQueue_preDispatchEvent from attempting to close
      // the keypad here to avoid a crash in android 4.2 + 4.3.
      if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN &&
          runtime->isActive()) {
        MAEvent *maEvent = new MAEvent();
        maEvent->nativeKey = AKEYCODE_BACK;
        maEvent->type = EVENT_TYPE_KEY_PRESSED;
        runtime->pushEvent(maEvent);
      }
      AInputQueue_finishEvent(app->inputQueue, event, true);
    } else if (!AInputQueue_preDispatchEvent(app->inputQueue, event)) {
      AInputQueue_finishEvent(app->inputQueue, event, handleInput(app, event));
    }
  }
}

int get_sensor_events(int fd, int events, void *data) {
  runtime->readSensorEvents();
  return 1;
}

// callback from MainActivity.java
extern "C" JNIEXPORT jboolean JNICALL Java_net_sourceforge_smallbasic_MainActivity_optionSelected
  (JNIEnv *env, jclass jclazz, jint index) {
  MAEvent *maEvent = new MAEvent();
  maEvent->type = EVENT_TYPE_OPTIONS_BOX_BUTTON_CLICKED;
  maEvent->optionsBoxButtonIndex = index;
  runtime->pushEvent(maEvent);
  return true;
}

extern "C" JNIEXPORT void JNICALL Java_net_sourceforge_smallbasic_MainActivity_runFile
  (JNIEnv *env, jclass jclazz, jstring path) {
  const char *fileName = env->GetStringUTFChars(path, JNI_FALSE);
  runtime->runPath(fileName);
  env->ReleaseStringUTFChars(path, fileName);
}

extern "C" JNIEXPORT void JNICALL Java_net_sourceforge_smallbasic_MainActivity_onResize
  (JNIEnv *env, jclass jclazz, jint width, jint height) {
  if (runtime != NULL && !runtime->isClosing() && runtime->isActive() && os_graphics) {
    runtime->onResize(width, height);
  }
}

extern "C" JNIEXPORT void JNICALL Java_net_sourceforge_smallbasic_MainActivity_onUnicodeChar
  (JNIEnv *env, jclass jclazz, jint ch) {
  if (runtime != NULL && !runtime->isClosing() && runtime->isActive() && os_graphics) {
    runtime->onUnicodeChar(ch);
  }
}

void onContentRectChanged(ANativeActivity *activity, const ARect *rect) {
  logEntered();
  runtime->onResize(rect->right, rect->bottom);
}

Runtime::Runtime(android_app *app) :
  System(),
  _keypadActive(false),
  _hasFocus(false),
  _graphics(NULL),
  _app(app),
  _eventQueue(NULL),
  _sensor(NULL),
  _sensorEventQueue(NULL) {
  _app->userData = NULL;
  _app->onAppCmd = handleCommand;
  _app->onInputEvent = handleInput;
  _app->inputPollSource.process = process_input;
  runtime = this;
  pthread_mutex_init(&_mutex, NULL);
  _looper = ALooper_forThread();
  _sensorManager = ASensorManager_getInstance();
}

Runtime::~Runtime() {
  logEntered();
  delete _output;
  delete _eventQueue;
  delete _graphics;
  runtime = NULL;
  _output = NULL;
  _eventQueue = NULL;
  _graphics = NULL;
  pthread_mutex_destroy(&_mutex);
  disableSensor();
}

void Runtime::alert(const char *title, const char *message) {
  logEntered();

  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jstring titleString = env->NewStringUTF(title);
  jstring messageString = env->NewStringUTF(message);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID method = env->GetMethodID(clazz, "showAlert",
                                      "(Ljava/lang/String;Ljava/lang/String;)V");
  env->CallVoidMethod(_app->activity->clazz, method, titleString, messageString);
  env->DeleteLocalRef(clazz);
  env->DeleteLocalRef(messageString);
  env->DeleteLocalRef(titleString);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::alert(const char *title, bool longDuration) {
  logEntered();

  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jstring titleString = env->NewStringUTF(title);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID method = env->GetMethodID(clazz, "showToast", "(Ljava/lang/String;Z)V");
  env->CallVoidMethod(_app->activity->clazz, method, titleString, longDuration);
  env->DeleteLocalRef(clazz);
  env->DeleteLocalRef(titleString);
  _app->activity->vm->DetachCurrentThread();
}

int Runtime::ask(const char *title, const char *prompt, bool cancel) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jstring titleString = env->NewStringUTF(title);
  jstring promptString = env->NewStringUTF(prompt);
  jmethodID methodId = env->GetMethodID(clazz, "ask",
                                        "(Ljava/lang/String;Ljava/lang/String;Z)I");
  jint result = (jint) env->CallIntMethod(_app->activity->clazz, methodId,
                                          titleString, promptString, cancel);
  env->DeleteLocalRef(clazz);
  env->DeleteLocalRef(titleString);
  env->DeleteLocalRef(promptString);
  _app->activity->vm->DetachCurrentThread();
  return result;
}

void Runtime::clearSoundQueue() {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, "clearSoundQueue", "()V");
  env->CallVoidMethod(_app->activity->clazz, methodId);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::construct() {
  logEntered();
  _state = kClosingState;
  _graphics = new Graphics(_app);
  if (_graphics && _graphics->construct(getFontId())) {
    int w = ANativeWindow_getWidth(_app->window);
    int h = ANativeWindow_getHeight(_app->window);
    _output = new AnsiWidget(w, h);
    if (_output && _output->construct()) {
      _eventQueue = new Stack<MAEvent *>();
      if (_eventQueue) {
        _state = kActiveState;
      }
    }
  }
}

void Runtime::disableSensor() {
  logEntered();
  if (_sensorEventQueue) {
    if (_sensor) {
      ASensorEventQueue_disableSensor(_sensorEventQueue, _sensor);
    }
    ASensorManager_destroyEventQueue(_sensorManager, _sensorEventQueue);
  }
  _sensorEventQueue = NULL;
  _sensor = NULL;
}

bool Runtime::enableSensor(int sensorType) {
  _sensorEvent.type = 0;
  if (!_sensorEventQueue) {
    _sensorEventQueue =
      ASensorManager_createEventQueue(_sensorManager, _looper, ALOOPER_POLL_CALLBACK,
                                      get_sensor_events, NULL);
  } else if (_sensor) {
    ASensorEventQueue_disableSensor(_sensorEventQueue, _sensor);
  }
  _sensor = ASensorManager_getDefaultSensor(_sensorManager, sensorType);
  bool result;
  if (_sensor) {
    ASensorEventQueue_enableSensor(_sensorEventQueue, _sensor);
    result = true;
  } else {
    result = false;
  }
  return result;
}

bool Runtime::getBoolean(const char *methodName) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, methodName, "()Z");
  jboolean result = (jboolean) env->CallBooleanMethod(_app->activity->clazz, methodId);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
  return result;
}

String Runtime::getString(const char *methodName) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, methodName, "()Ljava/lang/String;");
  jstring resultObj = (jstring)env->CallObjectMethod(_app->activity->clazz, methodId);
  const char *resultStr = env->GetStringUTFChars(resultObj, JNI_FALSE);
  String result = resultStr;
  env->ReleaseStringUTFChars(resultObj, resultStr);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
  return result;
}

String Runtime::getStringBytes(const char *methodName) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, methodName, "()[B");
  jbyteArray valueByteArray = (jbyteArray)env->CallObjectMethod(_app->activity->clazz, methodId);
  jsize len = env->GetArrayLength(valueByteArray);
  String result;
  if (len) {
    jbyte *buffer = new jbyte[len + 1];
    env->GetByteArrayRegion(valueByteArray, 0, len, buffer);
    buffer[len] = '\0';
    result = (const char *)buffer;
    delete [] buffer;
  }
  env->DeleteLocalRef(valueByteArray);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
  return result;
}

int Runtime::getInteger(const char *methodName) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, methodName, "()I");
  jint result = env->CallIntMethod(_app->activity->clazz, methodId);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
  return result;
}

int Runtime::getUnicodeChar(int keyCode, int metaState) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, "getUnicodeChar", "(II)I");
  jint result = env->CallIntMethod(_app->activity->clazz, methodId, keyCode, metaState);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
  return result;
}

char *Runtime::loadResource(const char *fileName) {
  char *buffer = System::loadResource(fileName);
  if (buffer == NULL && strcmp(fileName, MAIN_BAS) == 0) {
    AAssetManager *assetManager = _app->activity->assetManager;
    AAsset *mainBasFile = AAssetManager_open(assetManager, "main.bas", AASSET_MODE_BUFFER);
    off_t len = AAsset_getLength(mainBasFile);
    buffer = (char *)malloc(len + 1);
    if (AAsset_read(mainBasFile, buffer, len) < 0) {
      trace("failed to read main.bas");
    }
    buffer[len] = '\0';
    trace("loaded main.bas [%ld] bytes", len);
    AAsset_close(mainBasFile);
  }
  return buffer;
}

MAEvent *Runtime::popEvent() {
  pthread_mutex_lock(&_mutex);
  MAEvent *result = _eventQueue->pop();
  pthread_mutex_unlock(&_mutex);
  return result;
}

void Runtime::pushEvent(MAEvent *event) {
  pthread_mutex_lock(&_mutex);
  _eventQueue->push(event);
  pthread_mutex_unlock(&_mutex);
}

void Runtime::readSensorEvents() {
  ASensorEventQueue_getEvents(_sensorEventQueue, &_sensorEvent, 1);
}

void Runtime::setFloat(const char *methodName, float value) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, methodName, "(F)V");
  env->CallVoidMethod(_app->activity->clazz, methodId, value);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::setLocationData(var_t *retval) {
  String location = runtime->getString("getLocation");
  map_parse_str(location.c_str(), location.length(), retval);
}

void Runtime::setSensorData(var_t *retval) {
  v_init(retval);
  map_init(retval);
  if (_sensor != NULL) {
    v_setstr(map_add_var(retval, "name", 0), ASensor_getName(_sensor));
    switch (_sensorEvent.type) {
    case ASENSOR_TYPE_ACCELEROMETER:
    case ASENSOR_TYPE_MAGNETIC_FIELD:
    case ASENSOR_TYPE_GYROSCOPE:
      v_setreal(map_add_var(retval, "x", 0), _sensorEvent.vector.x);
      v_setreal(map_add_var(retval, "y", 0), _sensorEvent.vector.y);
      v_setreal(map_add_var(retval, "z", 0), _sensorEvent.vector.z);
      break;
    case ASENSOR_TYPE_LIGHT:
      v_setreal(map_add_var(retval, "light", 0), _sensorEvent.light);
      break;
    case ASENSOR_TYPE_PROXIMITY:
      v_setreal(map_add_var(retval, "distance", 0), _sensorEvent.distance);
      break;
    default:
      break;
    }
  }
}

void Runtime::runShell() {
  logEntered();

  opt_ide = IDE_NONE;
  opt_graphics = true;
  opt_pref_bpp = 0;
  opt_nosave = true;
  opt_interactive = true;
  opt_verbose = false;
  opt_quiet = true;
  opt_command[0] = 0;
  opt_usevmt = 0;
  opt_file_permitted = 1;
  os_graphics = 1;
  os_color_depth = 16;
  opt_mute_audio = 0;
  opt_loadmod = 0;
  strcpy(opt_modlist, "/data/data/net.sourceforge.smallbasic/lib");

  _app->activity->callbacks->onContentRectChanged = onContentRectChanged;
  loadConfig();

  String ipAddress = getString("getIPAddress");
  if (ipAddress.length()) {
    setenv("IP_ADDR", ipAddress.c_str(), 1);
  }

  String startupBas = getString("getStartupBas");
  if (startupBas.length()) {
    if (getBoolean("getUntrusted")) {
      opt_file_permitted = 0;
    }
    runOnce(startupBas.c_str());
  } else {
    runMain(MAIN_BAS);
  }
  saveConfig();

  _state = kDoneState;
  logLeaving();
}

void Runtime::loadConfig() {
  Properties settings;
  int fontSize = getInteger("getStartupFontSize");
  trace("fontSize = %d", fontSize);

  _output->setTextColor(DEFAULT_FOREGROUND, DEFAULT_BACKGROUND);
  _output->setFontSize(fontSize);
  _initialFontSize = _output->getFontSize();
  chdir("/sdcard");

  if (loadSettings(settings)) {
    String *s = settings.get(FONT_SCALE_KEY);
    if (s) {
      _fontScale = s->toInteger();
      trace("_fontScale = %d", _fontScale);
      if (_fontScale != 100) {
        int fontSize = (_initialFontSize * _fontScale / 100);
        _output->setFontSize(fontSize);
      }
    }
    s = settings.get(PATH_KEY);
    if (s) {
      trace("path = %s", s->c_str());
      chdir(s->c_str());
    }
    s = settings.get(MUTE_AUDIO_KEY);
    if (s && s->toInteger() == 1) {
      opt_mute_audio = 1;
    }
    s = settings.get(OPT_IDE_KEY);
    if (s) {
      opt_ide = s->toInteger();
    }
    loadEnvConfig(settings, SERVER_SOCKET_KEY);
    loadEnvConfig(settings, SERVER_TOKEN_KEY);
    loadEnvConfig(settings, FONT_ID_KEY);
  }
}

bool Runtime::loadSettings(Properties &settings) {
  bool result;
  String path;

  path.append(_app->activity->internalDataPath);
  path.append(CONFIG_FILE);
  FILE *fp = fopen(path.c_str(), "r");
  if (fp) {
    String buffer;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);
    buffer.append(fp, len);
    fclose(fp);
    settings.load(buffer.c_str(), buffer.length());
    result = true;
  } else {
    result = false;
  }
  return result;
}

void Runtime::loadEnvConfig(Properties &settings, const char *key) {
  String *s = settings.get(key);
  if (s) {
    trace("%s = %s", key, s->c_str());
    setenv(key, s->c_str(), 1);
  }
}

void Runtime::saveConfig() {
  String path;
  path.append(_app->activity->internalDataPath);
  path.append(CONFIG_FILE);
  FILE *fp = fopen(path.c_str(), "w");
  if (fp) {
    char path[FILENAME_MAX + 1];
    getcwd(path, FILENAME_MAX);
    fprintf(fp, "%s='%s'\n", PATH_KEY, path);
    fprintf(fp, "%s=%d\n", FONT_SCALE_KEY, _fontScale);
    fprintf(fp, "%s=%d\n", MUTE_AUDIO_KEY, opt_mute_audio);
    fprintf(fp, "%s=%d\n", OPT_IDE_KEY, opt_ide);
    for (int i = 0; environ[i] != NULL; i++) {
      char *env = environ[i];
      if (strstr(env, SERVER_SOCKET_KEY) != NULL ||
          strstr(env, SERVER_TOKEN_KEY) != NULL ||
          strstr(env, FONT_ID_KEY) != NULL) {
        fprintf(fp, "%s\n", env);
      }
    }
    fclose(fp);
  }
}

void Runtime::runPath(const char *path) {
  pthread_mutex_lock(&_mutex);
  setLoadPath(path);
  setExit(false);
  ALooper_wake(_looper);
  pthread_mutex_unlock(&_mutex);
}

void Runtime::handleKeyEvent(MAEvent &event) {
  switch (event.nativeKey) {
  case AKEYCODE_ENDCALL:
    delete [] _systemMenu;
    _systemMenu = NULL;
    break;
  case AKEYCODE_BACK:
    if (_keypadActive) {
      showKeypad(false);
    } else {
      setBack();
    }
    break;
  case AKEYCODE_MENU:
    showMenu();
    event.key = SB_KEY_MENU;
    break;
  case AKEYCODE_TAB:
    event.key = SB_KEY_TAB;
    break;
  case AKEYCODE_HOME:
    event.key = SB_KEY_KP_HOME;
    break;
  case AKEYCODE_MOVE_END:
    event.key = SB_KEY_END;
    break;
  case AKEYCODE_INSERT:
    event.key = SB_KEY_INSERT;
    break;
  case AKEYCODE_NUMPAD_MULTIPLY:
    event.key = SB_KEY_KP_MUL;
    break;
  case AKEYCODE_NUMPAD_ADD:
    event.key = SB_KEY_KP_PLUS;
    break;
  case AKEYCODE_NUMPAD_SUBTRACT:
    event.key = SB_KEY_KP_MINUS;
    break;
  case AKEYCODE_PAGE_UP:
    event.key = SB_KEY_PGUP;
    break;
  case AKEYCODE_PAGE_DOWN:
    event.key = SB_KEY_PGDN;
    break;
  case AKEYCODE_DPAD_UP:
    event.key = SB_KEY_UP;
    break;
  case AKEYCODE_DPAD_DOWN:
    event.key = SB_KEY_DN;
    break;
  case AKEYCODE_DPAD_LEFT:
    event.key = SB_KEY_LEFT;
    break;
  case AKEYCODE_DPAD_RIGHT:
    event.key = SB_KEY_RIGHT;
    break;
  case AKEYCODE_CLEAR:
    event.key = SB_KEY_DELETE;
    break;
  case AKEYCODE_DEL:
    event.key = SB_KEY_BACKSPACE;
    break;
  case AKEYCODE_ENTER:
    event.key = SB_KEY_ENTER;
    break;
  case GBOARD_KEY_QUESTION:
    event.key = '?';
    break;
  default:
    if (event.nativeKey < 127 && event.nativeKey != event.key) {
      // avoid translating keys send from onUnicodeChar
      event.key = getUnicodeChar(event.nativeKey, event.key);
    }
    break;
  }
  trace("native:%d sb:%d", event.nativeKey, event.key);
  if (isRunning() && event.key) {
    dev_pushkey(event.key);
  }
}

void Runtime::optionsBox(StringList *items) {
  logEntered();

  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);

  jstring elem = env->NewStringUTF(items->get(0)->c_str());
  jclass stringClass = env->GetObjectClass(elem);
  jobjectArray array = env->NewObjectArray(items->size(), stringClass, elem);
  for (int i = 1; i < items->size(); i++) {
    elem = env->NewStringUTF(items->get(i)->c_str());
    env->SetObjectArrayElement(array, i, elem);
  }
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, "optionsBox", "([Ljava/lang/String;)V");
  env->CallVoidMethod(_app->activity->clazz, methodId, array);

  for (int i = 0; i < items->size(); i++) {
    env->DeleteLocalRef(env->GetObjectArrayElement(array, i));
  }
  env->DeleteLocalRef(clazz);
  env->DeleteLocalRef(array);
  env->DeleteLocalRef(stringClass);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::playTone(int frq, int dur, int vol, bool bgplay) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, "playTone", "(III)V");
  env->CallVoidMethod(_app->activity->clazz, methodId, frq, dur, vol);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::pause(int timeout) {
  if (timeout == -1) {
    pollEvents(true);
    if (hasEvent()) {
      MAEvent *event = popEvent();
      processEvent(*event);
      delete event;
    }
  } else {
    int slept = 0;
    while (1) {
      pollEvents(false);
      if (isBreak()) {
        break;
      } else if (hasEvent()) {
        MAEvent *event = popEvent();
        processEvent(*event);
        delete event;
      }
      usleep(WAIT_INTERVAL * 1000);
      slept += WAIT_INTERVAL;
      if (timeout > 0 && slept > timeout) {
        break;
      }
    }
  }
}

void Runtime::pollEvents(bool blocking) {
  int events;
  android_poll_source *source;
  ALooper_pollAll(blocking || !_hasFocus ? -1 : 0, NULL, &events, (void **)&source);
  if (source != NULL) {
    source->process(_app, source);
  }
  if (_app->destroyRequested != 0) {
    trace("Thread destroy requested");
    setExit(true);
  }
}

MAEvent Runtime::processEvents(int waitFlag) {
  switch (waitFlag) {
  case 1:
    // wait for an event
    _output->flush(true);
    pollEvents(true);
    break;
  case 2:
    _output->flush(false);
    pause(WAIT_INTERVAL);
    break;
  default:
    pollEvents(false);
  }

  MAEvent event;
  if (hasEvent()) {
    MAEvent *nextEvent = popEvent();
    processEvent(*nextEvent);
    event = *nextEvent;
    delete nextEvent;
  } else {
    event.type = 0;
  }
  return event;
}

void Runtime::processEvent(MAEvent &event) {
  switch (event.type) {
  case EVENT_TYPE_SCREEN_CHANGED:
    _graphics->resize();
    resize();
    break;
  case EVENT_TYPE_KEY_PRESSED:
    handleKeyEvent(event);
    break;
  default:
    handleEvent(event);
    break;
  }
}

void Runtime::setString(const char *methodName, const char *value) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jstring valueString = env->NewStringUTF(value);
  jmethodID methodId = env->GetMethodID(clazz, methodName, "(Ljava/lang/String;)V");
  env->CallVoidMethod(_app->activity->clazz, methodId, valueString);
  env->DeleteLocalRef(valueString);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::setStringBytes(const char *methodName, const char *value) {
  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  int size = strlen(value);
  jbyteArray valueByteArray = env->NewByteArray(size);
  env->SetByteArrayRegion(valueByteArray, 0, size, (const jbyte *)value);
  jmethodID methodId = env->GetMethodID(clazz, methodName, "([B)V");
  env->CallVoidMethod(_app->activity->clazz, methodId, valueByteArray);
  env->DeleteLocalRef(valueByteArray);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::showKeypad(bool show) {
  logEntered();
  _keypadActive = show;

  JNIEnv *env;
  _app->activity->vm->AttachCurrentThread(&env, NULL);
  jclass clazz = env->GetObjectClass(_app->activity->clazz);
  jmethodID methodId = env->GetMethodID(clazz, "showKeypad", "(Z)V");
  env->CallVoidMethod(_app->activity->clazz, methodId, show);
  env->DeleteLocalRef(clazz);
  _app->activity->vm->DetachCurrentThread();
}

void Runtime::onResize(int width, int height) {
  logEntered();
  if (_graphics != NULL) {
    int w = _graphics->getWidth();
    int h = _graphics->getHeight();
    if (w != width || h != height) {
      trace("Resized from %d %d to %d %d", w, h, width, height);
      ALooper_acquire(_app->looper);
      _graphics->setSize(width, height);
      MAEvent *maEvent = new MAEvent();
      maEvent->type = EVENT_TYPE_SCREEN_CHANGED;
      runtime->pushEvent(maEvent);
      ALooper_wake(_app->looper);
      ALooper_release(_app->looper);
    }
  }
}

void Runtime::onUnicodeChar(int ch) {
  MAEvent *maEvent = new MAEvent();
  maEvent->type = EVENT_TYPE_KEY_PRESSED;
  maEvent->nativeKey = ch;
  maEvent->key = ch;
  ALooper_acquire(_app->looper);
  pushEvent(maEvent);
  ALooper_wake(_app->looper);
  ALooper_release(_app->looper);
}

char *Runtime::getClipboardText() {
  char *result;
  String text = getStringBytes("getClipboardText");
  if (text.length()) {
    result = strdup(text.c_str());
  } else {
    result = NULL;
  }
  return result;
}

int Runtime::getFontId() {
  int result = 0;
  Properties settings;
  if (loadSettings(settings)) {
    String *s = settings.get(FONT_ID_KEY);
    if (s) {
      result = s->toInteger();
    }
  }
  return result;
}

//
// System platform methods
//
bool System::getPen3() {
  bool result = false;
  if (_touchX != -1 && _touchY != -1) {
    result = true;
  } else {
    // get mouse
    processEvents(0);
    if (_touchX != -1 && _touchY != -1) {
      result = true;
    }
  }
  return result;
}

void System::completeKeyword(int index) {
  if (get_focus_edit() && isEditing()) {
    const char *help = get_focus_edit()->completeKeyword(index);
    if (help) {
      runtime->alert(help);
      runtime->getOutput()->redraw();
    }
  }
}

void System::editSource(strlib::String loadPath) {
  logEntered();

  strlib::String fileName;
  int i = loadPath.lastIndexOf('/', 0);
  if (i != -1) {
    fileName = loadPath.substring(i + 1);
  } else {
    fileName = loadPath;
  }

  strlib::String dirtyFile;
  dirtyFile.append(" * ");
  dirtyFile.append(fileName);
  strlib::String cleanFile;
  cleanFile.append(" - ");
  cleanFile.append(fileName);

  int w = _output->getWidth();
  int h = _output->getHeight();
  int charWidth = _output->getCharWidth();
  int charHeight = _output->getCharHeight();
  int prevScreenId = _output->selectScreen(SOURCE_SCREEN);
  TextEditInput *editWidget = new TextEditInput(_programSrc, charWidth, charHeight, 0, 0, w, h);
  TextEditHelpWidget *helpWidget = new TextEditHelpWidget(editWidget, charWidth, charHeight, false);
  TextEditInput *widget = editWidget;
  _modifiedTime = getModifiedTime();
  editWidget->updateUI(NULL, NULL);
  editWidget->setLineNumbers();
  editWidget->setFocus(true);
  if (strcmp(gsb_last_file, loadPath.c_str()) == 0) {
    editWidget->setCursorRow(gsb_last_line - 1);
  }
  if (gsb_last_error && !isBack()) {
    editWidget->setCursorRow(gsb_last_line - 1);
    runtime->alert(gsb_last_errmsg);
  }
  _srcRendered = false;
  _output->clearScreen();
  _output->addInput(editWidget);
  _output->addInput(helpWidget);
  _output->setStatus(cleanFile);
  _output->redraw();
  _state = kEditState;
  runtime->showKeypad(true);

  while (_state == kEditState) {
    MAEvent event = getNextEvent();
    switch (event.type) {
    case EVENT_TYPE_OPTIONS_BOX_BUTTON_CLICKED:
      if (editWidget->isDirty()) {
        _output->setStatus(dirtyFile);
        _output->redraw();
      }
      break;
    case EVENT_TYPE_KEY_PRESSED:
      if (_userScreenId == -1) {
        dev_clrkb();
        int sw = _output->getScreenWidth();
        bool redraw = true;
        bool dirty = editWidget->isDirty();
        char *text;

        switch (event.key) {
        case SB_KEY_MENU:
          redraw = false;
          break;
        case SB_KEY_F(1):
          widget = helpWidget;
          helpWidget->createKeywordIndex();
          helpWidget->show();
          helpWidget->setFocus(true);
          runtime->showKeypad(false);
          break;
        case SB_KEY_F(9):
          _state = kRunState;
          if (editWidget->isDirty()) {
            saveFile(editWidget, loadPath);
          }
          break;
        case SB_KEY_CTRL('s'):
          saveFile(editWidget, loadPath);
          break;
        case SB_KEY_CTRL('c'):
        case SB_KEY_CTRL('x'):
          text = widget->copy(event.key == (int)SB_KEY_CTRL('x'));
        if (text) {
          setClipboardText(text);
          free(text);
        }
        break;
        case SB_KEY_CTRL('v'):
          text = getClipboardText();
          widget->paste(text);
          free(text);
          break;
        case SB_KEY_CTRL('o'):
          _output->selectScreen(USER_SCREEN1);
          showCompletion(true);
          _output->redraw();
          _state = kActiveState;
          waitForBack();
          runtime->showKeypad(true);
          _output->selectScreen(SOURCE_SCREEN);
          _state = kEditState;
          break;
        default:
          redraw = widget->edit(event.key, sw, charWidth);
          break;
        }
        if (widget->isDirty() && !dirty) {
          _output->setStatus(dirtyFile);
        } else if (!widget->isDirty() && dirty) {
          _output->setStatus(cleanFile);
        }
        if (redraw) {
          _output->redraw();
        }
      }
    }

    if (isBack() && widget == helpWidget) {
      runtime->showKeypad(true);
      widget = editWidget;
      helpWidget->hide();
      editWidget->setFocus(true);
      _state = kEditState;
      _output->redraw();
    }

    if (widget->isDirty()) {
      int choice = -1;
      if (isClosing()) {
        choice = 0;
      } else if (isBack()) {
        const char *message = "The current file has not been saved.\n"
                              "Would you like to save it now?";
        choice = ask("Save changes?", message, isBack());
      }
      if (choice == 0) {
        widget->save(loadPath);
      } else if (choice == 2) {
        // cancel
        _state = kEditState;
      }
    }
  }

  _output->removeInputs();
  if (!isClosing()) {
    _output->selectScreen(prevScreenId);
  }
  logLeaving();
}

//
// ma event handling
//
int maGetEvent(MAEvent *event) {
  int result;
  if (runtime->hasEvent()) {
    MAEvent *nextEvent = runtime->popEvent();
    event->point = nextEvent->point;
    event->type = nextEvent->type;
    delete nextEvent;
    result = 1;
  } else {
    result = 0;
  }
  return result;
}

void maWait(int timeout) {
  runtime->pause(timeout);
}

int maShowVirtualKeyboard(void) {
  runtime->showKeypad(true);
  return 0;
}

//
// sbasic implementation
//
int osd_devinit(void) {
  runtime->setRunning(true);
  return 1;
}

int osd_devrestore(void) {
  runtime->setRunning(false);
  return 0;
}

void osd_audio(const char *path) {
  runtime->playAudio(path);
}

void osd_sound(int frq, int dur, int vol, int bgplay) {
  runtime->playTone(frq, dur, vol, bgplay);
}

void osd_clear_sound_queue() {
  runtime->clearSoundQueue();
}

void osd_beep(void) {
  osd_sound(1000, 30, 100, 0);
  osd_sound(500, 30, 100, 0);
}

//
// module implementation
//
int gps_on(int param_count, slib_par_t *params, var_t *retval) {
  runtime->getBoolean("requestLocationUpdates");
  return 1;
}

int gps_off(int param_count, slib_par_t *params, var_t *retval) {
  runtime->getBoolean("removeLocationUpdates");
  return 1;
}

int sensor_on(int param_count, slib_par_t *params, var_t *retval) {
  int result = 0;
  if (param_count == 1) {
    switch (v_getint(params[0].var_p)) {
    case 0:
      result = runtime->enableSensor(ASENSOR_TYPE_ACCELEROMETER);
      break;
    case 1:
      result = runtime->enableSensor(ASENSOR_TYPE_MAGNETIC_FIELD);
      break;
    case 2:
      result = runtime->enableSensor(ASENSOR_TYPE_GYROSCOPE);
      break;
    case 3:
      result = runtime->enableSensor(ASENSOR_TYPE_LIGHT);
      break;
    case 4:
      result = runtime->enableSensor(ASENSOR_TYPE_PROXIMITY);
      break;
    default:
      break;
    }
  }
  if (!result) {
    v_setstr(retval, "sensor not active");
  }
  return result;
}

int sensor_off(int param_count, slib_par_t *params, var_t *retval) {
  runtime->disableSensor();
  return 1;
}

int tts_speak(int param_count, slib_par_t *params, var_t *retval) {
  int result;
  if (param_count == 1 && v_is_type(params[0].var_p, V_STR)) {
    runtime->speak(v_getstr(params[0].var_p));
    result = 1;
  } else {
    v_setstr(retval, ERR_PARAM);
    result = 0;
  }
  return result;
}

int tts_pitch(int param_count, slib_par_t *params, var_t *retval) {
  int result;
  if (param_count == 1 && (v_is_type(params[0].var_p, V_NUM) ||
                           v_is_type(params[0].var_p, V_INT))) {
    runtime->setFloat("setTtsPitch", v_getreal(params[0].var_p));
    result = 1;
  } else {
    v_setstr(retval, ERR_PARAM);
    result = 0;
  }
  return result;
}

int tts_speech_rate(int param_count, slib_par_t *params, var_t *retval) {
  int result;
  if (param_count == 1 && (v_is_type(params[0].var_p, V_NUM) ||
                           v_is_type(params[0].var_p, V_INT))) {
    runtime->setFloat("setTtsRate", v_getreal(params[0].var_p));
    result = 1;
  } else {
    v_setstr(retval, ERR_PARAM);
    result = 0;
  }
  return result;
}

int tts_lang(int param_count, slib_par_t *params, var_t *retval) {
  int result;
  if (param_count == 1 && v_is_type(params[0].var_p, V_STR)) {
    runtime->setString("setTtsLocale", v_getstr(params[0].var_p));
    result = 1;
  } else {
    v_setstr(retval, ERR_PARAM);
    result = 0;
  }
  return result;
}

int tts_off(int param_count, slib_par_t *params, var_t *retval) {
  runtime->getBoolean("setTtsQuiet");
  return 1;
}

struct LibProcs {
  const char *name;
  int (*command)(int, slib_par_t *, var_t *retval);
} lib_procs[] = {
  {"GPS_ON", gps_on},
  {"GPS_OFF", gps_off},
  {"SENSOR_ON", sensor_on},
  {"SENSOR_OFF", sensor_off},
  {"TTS_PITCH", tts_pitch},
  {"TTS_RATE", tts_speech_rate},
  {"TTS_LANG", tts_lang},
  {"TTS_OFF", tts_off},
  {"SPEAK", tts_speak}
};

const char *sblib_get_module_name() {
  return "android";
}

int sblib_proc_count(void) {
  return (sizeof(lib_procs) / sizeof(lib_procs[0]));
}

int sblib_proc_getname(int index, char *proc_name) {
  int result;
  if (index < sblib_proc_count()) {
    strcpy(proc_name, lib_procs[index].name);
    result = 1;
  } else {
    result = 0;
  }
  return result;
}

int sblib_proc_exec(int index, int param_count, slib_par_t *params, var_t *retval) {
  int result;
  if (index < sblib_proc_count()) {
    result = lib_procs[index].command(param_count, params, retval);
  } else {
    result = 0;
  }
  return result;
}

const char *lib_funcs[] = {
  "LOCATION",
  "SENSOR"
};

int sblib_func_count(void) {
  return (sizeof(lib_funcs) / sizeof(lib_funcs[0]));
}

int sblib_func_getname(int index, char *proc_name) {
  int result;
  if (index < sblib_func_count()) {
    strcpy(proc_name, lib_funcs[index]);
    result = 1;
  } else {
    result = 0;
  }
  return result;
}

int sblib_func_exec(int index, int param_count, slib_par_t *params, var_t *retval) {
  int result;
  switch (index) {
  case 0:
    runtime->setLocationData(retval);
    result = 1;
    break;
  case 1:
    runtime->setSensorData(retval);
    result = 1;
    break;
  default:
    result = 0;
    break;
  }
  return result;
}

void sblib_close(void) {
  runtime->getBoolean("closeLibHandlers");
  runtime->disableSensor();
}
