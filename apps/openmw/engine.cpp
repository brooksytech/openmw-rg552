#include "engine.hpp"

#include <iomanip>
#include <chrono>
#include <thread>

#include <boost/filesystem/fstream.hpp>

#include <osg/Version>

#include <osgViewer/ViewerEventHandlers>
#include <osgDB/WriteFile>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#include <components/misc/rng.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/sdlgraphicswindow.hpp>
#include <components/sdlutil/imagetosurface.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/util.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"

#include "mwscript/scriptmanagerimp.hpp"
#include "mwscript/interpretercontext.hpp"

#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/player.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"

#include "mwstate/statemanagerimp.hpp"

namespace
{
    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    struct UserStats
    {
        const std::string mLabel;
        const std::string mBegin;
        const std::string mEnd;
        const std::string mTaken;

        UserStats(const std::string& label, const std::string& prefix)
            : mLabel(label),
              mBegin(prefix + "_time_begin"),
              mEnd(prefix + "_time_end"),
              mTaken(prefix + "_time_taken")
        {}
    };

    enum class UserStatsType : std::size_t
    {
        Input,
        Sound,
        State,
        Script,
        Mechanics,
        Physics,
        PhysicsWorker,
        World,
        Gui,
        Lua,

        Number,
    };

    template <UserStatsType type>
    struct UserStatsValue
    {
        static const UserStats sValue;
    };

    template <>
    const UserStats UserStatsValue<UserStatsType::Input>::sValue {"Input", "input"};

    template <>
    const UserStats UserStatsValue<UserStatsType::Sound>::sValue {"Sound", "sound"};

    template <>
    const UserStats UserStatsValue<UserStatsType::State>::sValue {"State", "state"};

    template <>
    const UserStats UserStatsValue<UserStatsType::Script>::sValue {"Script", "script"};

    template <>
    const UserStats UserStatsValue<UserStatsType::Mechanics>::sValue {"Mech", "mechanics"};

    template <>
    const UserStats UserStatsValue<UserStatsType::Physics>::sValue {"Phys", "physics"};

    template <>
    const UserStats UserStatsValue<UserStatsType::PhysicsWorker>::sValue {" -Async", "physicsworker"};

    template <>
    const UserStats UserStatsValue<UserStatsType::World>::sValue {"World", "world"};

    template <>
    const UserStats UserStatsValue<UserStatsType::Gui>::sValue {"Gui", "gui"};

    template <>
    const UserStats UserStatsValue<UserStatsType::Lua>::sValue {"Lua", "lua"};

    template <UserStatsType type>
    struct ForEachUserStatsValue
    {
        template <class F>
        static void apply(F&& f)
        {
            f(UserStatsValue<type>::sValue);
            using Next = ForEachUserStatsValue<static_cast<UserStatsType>(static_cast<std::size_t>(type) + 1)>;
            Next::apply(std::forward<F>(f));
        }
    };

    template <>
    struct ForEachUserStatsValue<UserStatsType::Number>
    {
        template <class F>
        static void apply(F&&) {}
    };

    template <class F>
    void forEachUserStatsValue(F&& f)
    {
        ForEachUserStatsValue<static_cast<UserStatsType>(0)>::apply(std::forward<F>(f));
    }

    template <UserStatsType sType>
    class ScopedProfile
    {
        public:
            ScopedProfile(osg::Timer_t frameStart, unsigned int frameNumber, const osg::Timer& timer, osg::Stats& stats)
                : mScopeStart(timer.tick()),
                  mFrameStart(frameStart),
                  mFrameNumber(frameNumber),
                  mTimer(timer),
                  mStats(stats)
            {
            }

            ScopedProfile(const ScopedProfile&) = delete;
            ScopedProfile& operator=(const ScopedProfile&) = delete;

            ~ScopedProfile()
            {
                if (!mStats.collectStats("engine"))
                    return;
                const osg::Timer_t end = mTimer.tick();
                const UserStats& stats = UserStatsValue<sType>::sValue;

                mStats.setAttribute(mFrameNumber, stats.mBegin, mTimer.delta_s(mFrameStart, mScopeStart));
                mStats.setAttribute(mFrameNumber, stats.mTaken, mTimer.delta_s(mScopeStart, end));
                mStats.setAttribute(mFrameNumber, stats.mEnd, mTimer.delta_s(mFrameStart, end));
            }

        private:
            const osg::Timer_t mScopeStart;
            const osg::Timer_t mFrameStart;
            const unsigned int mFrameNumber;
            const osg::Timer& mTimer;
            osg::Stats& mStats;
    };

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        forEachUserStatsValue([&] (const UserStats& v)
        {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier,
                                      average, averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::Manager::getInt("async num threads", "Physics") == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScheduleNonDialogMessageBox
    {
        void operator()(std::string message) const
        {
            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation() : GraphicsOperation("IdentifyOpenGLOperation", false)
        {}

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
        }
    };
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mEnvironment.getWorld()->getLocalScripts();

    localScripts.startIteration();
    std::pair<std::string, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext (
            &script.second.getRefData().getLocals(), script.second);
        mEnvironment.getScriptManager()->run (script.first, interpreterContext);
    }
}

bool OMW::Engine::frame(float frametime)
{
    try
    {
        const osg::Timer_t frameStart = mViewer->getStartTick();
        const unsigned int frameNumber = mViewer->getFrameStamp()->getFrameNumber();
        const osg::Timer* const timer = osg::Timer::instance();
        osg::Stats* const stats = mViewer->getViewerStats();

        mEnvironment.setFrameDuration(frametime);

        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mEnvironment.getInputManager()->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon changing widget textures (fixed in MyGUI 3.3.2),
        // and destroyed widgets will not be deleted (not fixed yet, https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            if (!mEnvironment.getWindowManager()->isWindowVisible())
            {
                mEnvironment.getSoundManager()->pausePlayback();
                return false;
            }
            else
                mEnvironment.getSoundManager()->resumePlayback();

            // sound
            if (mUseSound)
                mEnvironment.getSoundManager()->update(frametime);
        }

        // Main menu opened? Then scripts are also paused.
        bool paused = mEnvironment.getWindowManager()->containsMode(MWGui::GM_MainMenu);

        // Should be called after input manager update and before any change to the game world.
        // It applies to the game world queued changes from the previous frame.
        mLuaManager->synchronizedUpdate();

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mEnvironment.getStateManager()->update (frametime);
        }

        bool guiActive = mEnvironment.getWindowManager()->isGuiMode();

        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mEnvironment.getStateManager()->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!paused)
                {
                    if (mEnvironment.getWorld()->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mEnvironment.getScriptManager()->getGlobalScripts().run();
                    }

                    mEnvironment.getWorld()->markCellAsUnchanged();
                }

                if (!guiActive)
                {
                    double hours = (frametime * mEnvironment.getWorld()->getTimeScaleFactor()) / 3600.0;
                    mEnvironment.getWorld()->advanceTime(hours, true);
                    mEnvironment.getWorld()->rechargeItems(frametime, true);
                }
            }
        }

        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mEnvironment.getStateManager()->getState() != MWBase::StateManager::State_NoGame)
            {
                mEnvironment.getMechanicsManager()->update(frametime, guiActive);
            }

            if (mEnvironment.getStateManager()->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mEnvironment.getWorld()->getPlayerPtr();
                if(!guiActive && player.getClass().getCreatureStats(player).isDead())
                    mEnvironment.getStateManager()->endGame();
            }
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mEnvironment.getStateManager()->getState() != MWBase::StateManager::State_NoGame)
            {
                mEnvironment.getWorld()->updatePhysics(frametime, guiActive, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mEnvironment.getStateManager()->getState() != MWBase::StateManager::State_NoGame)
            {
                mEnvironment.getWorld()->update(frametime, guiActive);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mEnvironment.getWindowManager()->update(frametime);
        }

        if (stats->collectStats("resource"))
        {
            stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

            mResourceSystem->reportStats(frameNumber, stats);

            stats->setAttribute(frameNumber, "WorkQueue", mWorkQueue->getNumItems());
            stats->setAttribute(frameNumber, "WorkThread", mWorkQueue->getNumActiveThreads());

            mEnvironment.reportStats(frameNumber, *stats);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }
    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
  : mWindow(nullptr)
  , mEncoding(ToUTF8::WINDOWS_1252)
  , mEncoder(nullptr)
  , mScreenCaptureOperation(nullptr)
  , mSkipMenu (false)
  , mUseSound (true)
  , mCompileAll (false)
  , mCompileAllDialogue (false)
  , mWarningsMode (1)
  , mScriptConsoleMode (false)
  , mActivationDistanceOverride(-1)
  , mGrab(true)
  , mExportFonts(false)
  , mRandomSeed(0)
  , mScriptContext (nullptr)
  , mLuaManager (nullptr)
  , mFSStrict (false)
  , mScriptBlacklistUse (true)
  , mNewGame (false)
  , mCfgMgr(configurationManager)
{
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    Uint32 flags = SDL_INIT_VIDEO|SDL_INIT_NOPARACHUTE|SDL_INIT_GAMECONTROLLER|SDL_INIT_JOYSTICK|SDL_INIT_SENSOR;
    if(SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if(SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
        mScreenCaptureOperation->stop();

    mEnvironment.cleanup();

    delete mScriptContext;
    mScriptContext = nullptr;

    mWorkQueue = nullptr;

    mViewer = nullptr;

    mResourceSystem.reset();

    delete mEncoder;
    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();
}

void OMW::Engine::enableFSStrict(bool fsStrict)
{
    mFSStrict = fsStrict;
}

// Set data dir

void OMW::Engine::setDataDirs (const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), (mResDir / "vfs"));
    mFileCollections = Files::Collections (mDataDirs, !mFSStrict);
}

// Add BSA archive
void OMW::Engine::addArchive (const std::string& archive) {
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir (const boost::filesystem::path& parResDir)
{
    mResDir = parResDir;
}

// Set start cell name
void OMW::Engine::setCell (const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu (bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createWindow(Settings::Manager& settings)
{
    int screen = settings.getInt("screen", "Video");
    int width = settings.getInt("resolution x", "Video");
    int height = settings.getInt("resolution y", "Video");
    bool fullscreen = settings.getBool("fullscreen", "Video");
    bool windowBorder = settings.getBool("window border", "Video");
    bool vsync = settings.getBool("vsync", "Video");
    unsigned int antialiasing = std::max(0, settings.getInt("antialiasing", "Video"));

    int pos_x = SDL_WINDOWPOS_CENTERED_DISPLAY(screen),
        pos_y = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);

    if(fullscreen)
    {
        pos_x = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        pos_y = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    Uint32 flags = SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE;
    if(fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;

    // Allows for Windows snapping features to properly work in borderless window
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
                settings.getBool("minimize on focus loss", "Video") ? "1" : "0");

    checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
    if (Debug::shouldDebugOpenGL())
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

    if (antialiasing > 0)
    {
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
    }

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
    while (!graphicsWindow || !graphicsWindow->valid())
    {
        while (!mWindow)
        {
            mWindow = SDL_CreateWindow("OpenMW", pos_x, pos_y, width, height, flags);
            if (!mWindow)
            {
                // Try with a lower AA
                if (antialiasing > 0)
                {
                    Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying " << antialiasing/2;
                    antialiasing /= 2;
                    Settings::Manager::setInt("antialiasing", "Video", antialiasing);
                    checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                    continue;
                }
                else
                {
                    std::stringstream error;
                    error << "Failed to create SDL window: " << SDL_GetError();
                    throw std::runtime_error(error.str());
                }
            }
        }

        setWindowIcon();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
        SDL_GetWindowSize(mWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mWindow)&SDL_WINDOW_BORDERLESS);
        traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
        traits->vsync = vsync;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

        graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits);
        if (!graphicsWindow->valid()) throw std::runtime_error("Failed to create GraphicsContext");

        if (traits->samples < antialiasing)
        {
            Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of " << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
            graphicsWindow->closeImplementation();
            SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            antialiasing /= 2;
            Settings::Manager::setInt("antialiasing", "Video", antialiasing);
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            continue;
        }

        if (traits->red < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
        if (traits->green < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
        if (traits->blue < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
        if (traits->depth < 24)
            Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

        traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
    }

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    realizeOperations->add(new IdentifyOpenGLOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    mViewer->realize();

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    boost::filesystem::ifstream windowIconStream;
    std::string windowIcon = (mResDir / "mygui" / "openmw.png").string();
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code " << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine (Settings::Manager & settings)
{
    mEnvironment.setStateManager (
        new MWState::StateManager (mCfgMgr.getUserDataPath() / "saves", mContentFiles));

    createWindow(settings);

    osg::ref_ptr<osg::Group> rootNode (new osg::Group);
    mViewer->setSceneData(rootNode);

    mVFS.reset(new VFS::Manager(mFSStrict));

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true);

    mResourceSystem.reset(new Resource::ResourceSystem(mVFS.get()));
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(
        Settings::Manager::getString("texture mag filter", "General"),
        Settings::Manager::getString("texture min filter", "General"),
        Settings::Manager::getString("texture mipmap", "General"),
        Settings::Manager::getInt("anisotropy", "General")
    );

    int numThreads = Settings::Manager::getInt("preload num threads", "Cells");
    if (numThreads <= 0)
        throw std::runtime_error("Invalid setting: 'preload num threads' must be >0");
    mWorkQueue = new SceneUtil::WorkQueue(numThreads);

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(
        mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(
            mCfgMgr.getScreenshotPath().string(),
            Settings::Manager::getString("screenshot format", "General"),
            Settings::Manager::getBool("notify on saved screenshot", "General")
                    ? std::function<void (std::string)>(ScheduleNonDialogMessageBox {})
                    : std::function<void (std::string)>(IgnoreString {})
        )
    );

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mLuaManager = new MWLua::LuaManager(mVFS.get(), (mResDir / "lua_libs").string());
    mEnvironment.setLuaManager(mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    std::string keybinderUser = (mCfgMgr.getUserConfigPath() / "input_v3.xml").string();
    bool keybinderUserExists = boost::filesystem::exists(keybinderUser);
    if(!keybinderUserExists)
    {
        std::string input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml").string();
        if(boost::filesystem::exists(input2)) {
            boost::filesystem::copy_file(input2, keybinderUser);
            keybinderUserExists = boost::filesystem::exists(keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const std::string userdefault = mCfgMgr.getUserConfigPath().string() + "/gamecontrollerdb.txt";
    const std::string localdefault = mCfgMgr.getLocalPath().string() + "/gamecontrollerdb.txt";
    const std::string globaldefault = mCfgMgr.getGlobalPath().string() + "/gamecontrollerdb.txt";

    std::string userGameControllerdb;
    if (boost::filesystem::exists(userdefault)){
        userGameControllerdb = userdefault;
    }
    else
        userGameControllerdb = "";

    std::string gameControllerdb;
    if (boost::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (boost::filesystem::exists(globaldefault))
        gameControllerdb = globaldefault;
    else
        gameControllerdb = ""; //if it doesn't exist, pass in an empty string

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath((mResDir / "shaders").string());

    osg::ref_ptr<osg::GLExtensions> exts = osg::GLExtensions::Get(0, false);
    bool shadersSupported = exts && (exts->glslLanguageVersion >= 1.2f);
    bool enableReverseZ = false;

    if (Settings::Manager::getBool("reverse z", "Camera"))
    {
        if (exts && exts->isClipControlSupported)
        {
            enableReverseZ = true;
            Log(Debug::Info) << "Using reverse-z depth buffer";
        }
        else
            Log(Debug::Warning) << "GL_ARB_clip_control not supported: disabling reverse-z depth buffer";
    }
    else
        Log(Debug::Info) << "Using standard depth buffer";

    SceneUtil::AutoDepth::setReversed(enableReverseZ);

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (exts)
        exts->glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    std::string myguiResources = (mResDir / "mygui").string();
    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    rootNode->addChild(guiRoot);
    MWGui::WindowManager* window = new MWGui::WindowManager(mWindow, mViewer, guiRoot, mResourceSystem.get(), mWorkQueue.get(),
                mCfgMgr.getLogPath().string() + std::string("/"), myguiResources,
                mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts,
                Version::getOpenmwVersionDescription(mResDir.string()), mCfgMgr.getUserConfigPath().string(), shadersSupported);
    mEnvironment.setWindowManager (window);

    MWInput::InputManager* input = new MWInput::InputManager (mWindow, mViewer, mScreenCaptureHandler, mScreenCaptureOperation, keybinderUser, keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    mEnvironment.setInputManager (input);

    // Create sound system
    mEnvironment.setSoundManager (new MWSound::SoundManager(mVFS.get(), mUseSound));

    if (!mSkipMenu)
    {
        const std::string& logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            window->playVideo(logo, true);
    }

    // Create the world
    mEnvironment.setWorld( new MWWorld::World (mViewer, rootNode, mResourceSystem.get(), mWorkQueue.get(),
        mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder, mActivationDistanceOverride, mCellName,
        mStartupScript, mResDir.string(), mCfgMgr.getUserDataPath().string()));
    mEnvironment.getWorld()->setupPlayer();

    window->setStore(mEnvironment.getWorld()->getStore());
    window->initUI();

    //Load translation data
    mTranslationDataStorage.setEncoder(mEncoder);
    for (size_t i = 0; i < mContentFiles.size(); i++)
      mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFiles[i]);

    Compiler::registerExtensions (mExtensions);

    // Create script system
    mScriptContext = new MWScript::CompilerContext (MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions (&mExtensions);

    mEnvironment.setScriptManager (new MWScript::ScriptManager (mEnvironment.getWorld()->getStore(), *mScriptContext, mWarningsMode,
        mScriptBlacklistUse ? mScriptBlacklist : std::vector<std::string>()));

    // Create game mechanics system
    MWMechanics::MechanicsManager* mechanics = new MWMechanics::MechanicsManager;
    mEnvironment.setMechanicsManager (mechanics);

    // Create dialog system
    mEnvironment.setJournal (new MWDialogue::Journal);
    mEnvironment.setDialogueManager (new MWDialogue::DialogueManager (mExtensions, mTranslationDataStorage));
    mEnvironment.setResourceSystem(mResourceSystem.get());

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mEnvironment.getScriptManager()->compileAll();
        if (result.first)
            Log(Debug::Info)
                << "compiled " << result.second << " of " << result.first << " scripts ("
                << 100*static_cast<double> (result.second)/result.first
                << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info)
                << "compiled " << result.second << " of " << result.first << " dialogue script/actor combinations a("
                << 100*static_cast<double> (result.second)/result.first
                << "%)";
    }

    mLuaManager->init();
    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath().string());
}

class OMW::Engine::LuaWorker
{
public:
    explicit LuaWorker(Engine* engine) : mEngine(engine)
    {
        if (Settings::Manager::getInt("lua num threads", "Lua") > 0)
            mThread = std::thread([this]{ threadBody(); });
    };

    void allowUpdate()
    {
        if (!mThread)
            return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mUpdateRequest = true;
        }
        mCV.notify_one();
    }

    void finishUpdate()
    {
        if (mThread)
        {
            std::unique_lock<std::mutex> lk(mMutex);
            mCV.wait(lk, [&]{ return !mUpdateRequest; });
        }
        else
            update();
    };

    void join()
    {
        if (mThread)
        {
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mJoinRequest = true;
            }
            mCV.notify_one();
            mThread->join();
        }
    }

private:
    void update()
    {
        const auto& viewer = mEngine->mViewer;
        const osg::Timer_t frameStart = viewer->getStartTick();
        const unsigned int frameNumber = viewer->getFrameStamp()->getFrameNumber();
        ScopedProfile<UserStatsType::Lua> profile(frameStart, frameNumber, *osg::Timer::instance(), *viewer->getViewerStats());

        mEngine->mLuaManager->update();
    }

    void threadBody()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lk(mMutex);
            mCV.wait(lk, [&]{ return mUpdateRequest || mJoinRequest; });
            if (mJoinRequest)
                break;

            update();

            mUpdateRequest = false;
            lk.unlock();
            mCV.notify_one();
        }
    }

    Engine* mEngine;
    std::mutex mMutex;
    std::condition_variable mCV;
    bool mUpdateRequest = false;
    bool mJoinRequest = false;
    std::optional<std::thread> mThread;
};

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert (!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "." << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    // Load settings
    Settings::Manager settings;
    std::string settingspath = settings.load(mCfgMgr);

    MWClass::registerClasses();

    // Create encoder
    mEncoder = new ToUTF8::Utf8Encoder(mEncoding);

    // Setup viewer
    mViewer = new osgViewer::Viewer;
    mViewer->setReleaseContextAtEndOfFrameHint(false);

#if OSG_VERSION_GREATER_OR_EQUAL(3,5,5)
    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);
#endif

    mEnvironment.setFrameRateLimit(Settings::Manager::getFloat("framerate limit", "Video"));

    prepareEngine (settings);

    std::ofstream stats;
    if (const auto path = std::getenv("OPENMW_OSG_STATS_FILE"))
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "Stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file for stats: " << path;
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statshandler = new Resource::Profiler(stats.is_open());

    initStatsHandler(*statshandler);

    mViewer->addEventHandler(statshandler);

    osg::ref_ptr<Resource::StatsHandler> resourceshandler = new Resource::StatsHandler(stats.is_open());
    mViewer->addEventHandler(resourceshandler);

    if (stats.is_open())
        Resource::CollectStatistics(mViewer);

    // Start the game
    if (!mSaveGameFile.empty())
    {
        mEnvironment.getStateManager()->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        mEnvironment.getWindowManager()->pushGuiMode (MWGui::GM_MainMenu);
        mEnvironment.getSoundManager()->playTitleMusic();
        const std::string& logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mEnvironment.getWindowManager()->playVideo(logo, true);
    }
    else
    {
        mEnvironment.getStateManager()->newGame (!mNewGame);
    }

    if (!mStartupScript.empty() && mEnvironment.getStateManager()->getState() == MWState::StateManager::State_Running)
    {
        mEnvironment.getWindowManager()->executeInConsole(mStartupScript);
    }

    LuaWorker luaWorker(this);  // starts a separate lua thread if "lua num threads" > 0

    // Start the main rendering loop
    double simulationTime = 0.0;
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mViewer->done() && !mEnvironment.getStateManager()->hasQuitRequest())
    {
        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(std::min(
            frameRateLimiter.getLastFrameDuration(),
            maxSimulationInterval
        )).count();

        mViewer->advance(simulationTime);

        if (!frame(dt))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        else
        {
            mViewer->eventTraversal();
            mViewer->updateTraversal();

            mEnvironment.getWorld()->updateWindowManager();

            luaWorker.allowUpdate();  // if there is a separate Lua thread, it starts the update now

            mViewer->renderingTraversals();

            luaWorker.finishUpdate();

            bool guiActive = mEnvironment.getWindowManager()->isGuiMode();
            if (!guiActive)
                simulationTime += dt;
        }

        if (stats)
        {
            const auto frameNumber = mViewer->getFrameStamp()->getFrameNumber();
            if (frameNumber >= 2)
            {
                mViewer->getViewerStats()->report(stats, frameNumber - 2);
                osgViewer::Viewer::Cameras cameras;
                mViewer->getCameras(cameras);
                for (auto camera : cameras)
                    camera->getStats()->report(stats, frameNumber - 2);
            }
        }

        frameRateLimiter.limit();
    }

    luaWorker.join();

    // Save user settings
    settings.saveUser(settingspath);
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath().string());

    Log(Debug::Info) << "Quitting peacefully.";
}

void OMW::Engine::setCompileAll (bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue (bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode (bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript (const std::string& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride (int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode (int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::setScriptBlacklist (const std::vector<std::string>& list)
{
    mScriptBlacklist = list;
}

void OMW::Engine::setScriptBlacklistUse (bool use)
{
    mScriptBlacklistUse = use;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::string &savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}
