// Headless version of PPSSPP, for testing using http://code.google.com/p/pspautotests/ .
// See headless.txt.
// To build on non-windows systems, just run CMake in the SDL directory, it will build both a normal ppsspp and the headless version.
// Example command line to run a test in the VS debugger (useful to debug failures):
// > --root pspautotests/tests/../ --compare --timeout=5 --graphics=software pspautotests/tests/cpu/cpu_alu/cpu_alu.prx

#include "ppsspp_config.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#if PPSSPP_PLATFORM(ANDROID)
#include <jni.h>
#endif

#include "Common/Profiler/Profiler.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"

#include "Common/CommonWindows.h"
#if PPSSPP_PLATFORM(WINDOWS)
#include <timeapi.h>
#else
#include <csignal>
#endif
#include "Common/CPUDetect.h"
#include "Common/File/VFS/VFS.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Common/File/VFS/DirectoryReader.h"
#include "Common/File/FileUtil.h"
#include "Common/GraphicsContext.h"
#include "Common/TimeUtil.h"
#include "Common/Thread/ThreadManager.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/System.h"
#include "Core/WebServer.h"
#include "Core/HLE/sceUtility.h"
#include "Core/Host.h"
#include "Core/SaveState.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "Log.h"
#include "LogManager.h"

#include "Compare.h"
#include "StubHost.h"
#if defined(_WIN32)
#include "WindowsHeadlessHost.h"
#elif defined(SDL)
#include "SDLHeadlessHost.h"
#endif

#if PPSSPP_PLATFORM(ANDROID)
JNIEnv *getEnv() {
	return nullptr;
}

jclass findClass(const char *name) {
	return nullptr;
}

bool audioRecording_Available() { return false; }
bool audioRecording_State() { return false; }
#endif

class PrintfLogger : public LogListener {
public:
	void Log(const LogMessage &message) override {
		switch (message.level) {
		case LogTypes::LVERBOSE:
			fprintf(stderr, "V %s", message.msg.c_str());
			break;
		case LogTypes::LDEBUG:
			fprintf(stderr, "D %s", message.msg.c_str());
			break;
		case LogTypes::LINFO:
			fprintf(stderr, "I %s", message.msg.c_str());
			break;
		case LogTypes::LERROR:
			fprintf(stderr, "E %s", message.msg.c_str());
			break;
		case LogTypes::LWARNING:
			fprintf(stderr, "W %s", message.msg.c_str());
			break;
		case LogTypes::LNOTICE:
		default:
			fprintf(stderr, "N %s", message.msg.c_str());
			break;
		}
	}
};

// Temporary hacks around annoying linking errors.
void NativeUpdate() { }
void NativeRender(GraphicsContext *graphicsContext) { }
void NativeResized() { }

std::string System_GetProperty(SystemProperty prop) { return ""; }
std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) { return std::vector<std::string>(); }
int System_GetPropertyInt(SystemProperty prop) {
	if (prop == SYSPROP_SYSTEMVERSION)
		return 31;
	return -1;
}
float System_GetPropertyFloat(SystemProperty prop) { return -1.0f; }
bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
		case SYSPROP_CAN_JIT:
			return true;
		default:
			return false;
	}
}

void System_SendMessage(const char *command, const char *parameter) {}
void System_InputBoxGetString(const std::string &title, const std::string &defaultValue, std::function<void(bool, const std::string &)> cb) { cb(false, ""); }
void System_AskForPermission(SystemPermission permission) {}
PermissionStatus System_GetPermissionStatus(SystemPermission permission) { return PERMISSION_STATUS_GRANTED; }

int printUsage(const char *progname, const char *reason)
{
	if (reason != NULL)
		fprintf(stderr, "Error: %s\n\n", reason);
	fprintf(stderr, "PPSSPP Headless\n");
	fprintf(stderr, "This is primarily meant as a non-interactive test tool.\n\n");
	fprintf(stderr, "Usage: %s file.elf... [options]\n\n", progname);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -m, --mount umd.cso   mount iso on umd1:\n");
	fprintf(stderr, "  -r, --root some/path  mount path on host0: (elfs must be in here)\n");
	fprintf(stderr, "  -l, --log             full log output, not just emulated printfs\n");
	fprintf(stderr, "  --debugger=PORT       enable websocket debugger and break at start\n");

	fprintf(stderr, "  --graphics=BACKEND    use a different gpu backend\n");
	fprintf(stderr, "                        options: gles, software, directx9, etc.\n");
	fprintf(stderr, "  --screenshot=FILE     compare against a screenshot\n");
	fprintf(stderr, "  --max-mse=NUMBER      maximum allowed MSE error for screenshot\n");
	fprintf(stderr, "  --timeout=SECONDS     abort test it if takes longer than SECONDS\n");

	fprintf(stderr, "  -v, --verbose         show the full passed/failed result\n");
	fprintf(stderr, "  -i                    use the interpreter\n");
	fprintf(stderr, "  --ir                  use ir interpreter\n");
	fprintf(stderr, "  -j                    use jit (default)\n");
	fprintf(stderr, "  -c, --compare         compare with output in file.expected\n");
	fprintf(stderr, "  --bench               run multiple times and output speed\n");
	fprintf(stderr, "\nSee headless.txt for details.\n");

	return 1;
}

static HeadlessHost *getHost(GPUCore gpuCore) {
	switch (gpuCore) {
	case GPUCORE_SOFTWARE:
		return new HeadlessHost();
#ifdef HEADLESSHOST_CLASS
	default:
		return new HEADLESSHOST_CLASS();
#else
	default:
		return new HeadlessHost();
#endif
	}
}

struct AutoTestOptions {
	double timeout;
	double maxScreenshotError;
	bool compare : 1;
	bool verbose : 1;
	bool bench : 1;
};

bool RunAutoTest(HeadlessHost *headlessHost, CoreParameter &coreParameter, const AutoTestOptions &opt) {
	// Kinda ugly, trying to guesstimate the test name from filename...
	currentTestName = GetTestName(coreParameter.fileToStart);

	std::string output;
	if (opt.compare || opt.bench)
		coreParameter.collectEmuLog = &output;

	std::string error_string;
	if (!PSP_InitStart(coreParameter, &error_string)) {
		fprintf(stderr, "Failed to start '%s'. Error: %s\n", coreParameter.fileToStart.c_str(), error_string.c_str());
		printf("TESTERROR\n");
		TeamCityPrint("testIgnored name='%s' message='PRX/ELF missing'", currentTestName.c_str());
		GitHubActionsPrint("error", "PRX/ELF missing for %s", currentTestName.c_str());
		return false;
	}

	TeamCityPrint("testStarted name='%s' captureStandardOutput='true'", currentTestName.c_str());

	if (opt.compare)
		headlessHost->SetComparisonScreenshot(ExpectedScreenshotFromFilename(coreParameter.fileToStart), opt.maxScreenshotError);

	while (!PSP_InitUpdate(&error_string))
		sleep_ms(1);
	if (!PSP_IsInited()) {
		TeamCityPrint("testFailed name='%s' message='Startup failed'", currentTestName.c_str());
		TeamCityPrint("testFinished name='%s'", currentTestName.c_str());
		GitHubActionsPrint("error", "Test init failed for %s", currentTestName.c_str());
		return false;
	}

	host->BootDone();

	Core_UpdateDebugStats(g_Config.bShowDebugStats || g_Config.bLogFrameDrops);

	PSP_BeginHostFrame();
	Draw::DrawContext *draw = coreParameter.graphicsContext ? coreParameter.graphicsContext->GetDrawContext() : nullptr;
	if (draw)
		draw->BeginFrame();

	bool passed = true;
	double deadline = time_now_d() + opt.timeout;
	coreState = coreParameter.startBreak ? CORE_STEPPING : CORE_RUNNING;
	while (coreState == CORE_RUNNING || coreState == CORE_STEPPING)
	{
		int blockTicks = (int)usToCycles(1000000 / 10);
		PSP_RunLoopFor(blockTicks);

		// If we were rendering, this might be a nice time to do something about it.
		if (coreState == CORE_NEXTFRAME) {
			coreState = CORE_RUNNING;
			headlessHost->SwapBuffers();
		}
		if (coreState == CORE_STEPPING && !coreParameter.startBreak) {
			break;
		}
		if (time_now_d() > deadline) {
			// Don't compare, print the output at least up to this point, and bail.
			if (!opt.bench) {
				printf("%s", output.c_str());

				host->SendDebugOutput("TIMEOUT\n");
				TeamCityPrint("testFailed name='%s' message='Test timeout'", currentTestName.c_str());
				GitHubActionsPrint("error", "Test timeout for %s", currentTestName.c_str());
			}

			passed = false;
			Core_Stop();
		}
	}
	PSP_EndHostFrame();

	if (draw) {
		draw->BindFramebufferAsRenderTarget(nullptr, { Draw::RPAction::CLEAR, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE }, "Headless");
		// Vulkan may get angry if we don't do a final present.
		if (gpu)
			gpu->CopyDisplayToOutput(true);

		draw->EndFrame();
	}

	PSP_Shutdown();

	if (!opt.bench)
		headlessHost->FlushDebugOutput();

	if (opt.compare && passed)
		passed = CompareOutput(coreParameter.fileToStart, output, opt.verbose);

	TeamCityPrint("testFinished name='%s'", currentTestName.c_str());

	return passed;
}

int main(int argc, const char* argv[])
{
	PROFILE_INIT();
#if PPSSPP_PLATFORM(WINDOWS)
	timeBeginPeriod(1);
#else
	// Ignore sigpipe.
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		perror("Unable to ignore SIGPIPE");
	}
#endif

#if defined(_DEBUG) && defined(_MSC_VER)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	AutoTestOptions testOptions{};
	testOptions.timeout = std::numeric_limits<double>::infinity();
	bool fullLog = false;
	const char *stateToLoad = 0;
	GPUCore gpuCore = GPUCORE_SOFTWARE;
	CPUCore cpuCore = CPUCore::JIT;
	int debuggerPort = -1;

	std::vector<std::string> testFilenames;
	const char *mountIso = nullptr;
	const char *mountRoot = nullptr;
	const char *screenshotFilename = nullptr;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--mount"))
		{
			if (++i >= argc)
				return printUsage(argv[0], "Missing argument after -m");
			mountIso = argv[i];
		}
		else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--root"))
		{
			if (++i >= argc)
				return printUsage(argv[0], "Missing argument after -r");
			mountRoot = argv[i];
		}
		else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--log"))
			fullLog = true;
		else if (!strcmp(argv[i], "-i"))
			cpuCore = CPUCore::INTERPRETER;
		else if (!strcmp(argv[i], "-j"))
			cpuCore = CPUCore::JIT;
		else if (!strcmp(argv[i], "--ir"))
			cpuCore = CPUCore::IR_JIT;
		else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--compare"))
			testOptions.compare = true;
		else if (!strcmp(argv[i], "--bench"))
			testOptions.bench = true;
		else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			testOptions.verbose = true;
		else if (!strncmp(argv[i], "--graphics=", strlen("--graphics=")) && strlen(argv[i]) > strlen("--graphics="))
		{
			const char *gpuName = argv[i] + strlen("--graphics=");
			if (!strcasecmp(gpuName, "gles"))
				gpuCore = GPUCORE_GLES;
			// There used to be a separate "null" rendering core - just use software.
			else if (!strcasecmp(gpuName, "software") || !strcasecmp(gpuName, "null"))
				gpuCore = GPUCORE_SOFTWARE;
			else if (!strcasecmp(gpuName, "directx9"))
				gpuCore = GPUCORE_DIRECTX9;
			else if (!strcasecmp(gpuName, "directx11"))
				gpuCore = GPUCORE_DIRECTX11;
			else if (!strcasecmp(gpuName, "vulkan"))
				gpuCore = GPUCORE_VULKAN;
			else
				return printUsage(argv[0], "Unknown gpu backend specified after --graphics=. Allowed: software, directx9, directx11, vulkan, gles, null.");
		}
		// Default to GLES if no value selected.
		else if (!strcmp(argv[i], "--graphics")) {
#if PPSSPP_API(ANY_GL)
			gpuCore = GPUCORE_GLES;
#else
			gpuCore = GPUCORE_DIRECTX11;
#endif
		} else if (!strncmp(argv[i], "--screenshot=", strlen("--screenshot=")) && strlen(argv[i]) > strlen("--screenshot="))
			screenshotFilename = argv[i] + strlen("--screenshot=");
		else if (!strncmp(argv[i], "--timeout=", strlen("--timeout=")) && strlen(argv[i]) > strlen("--timeout="))
			testOptions.timeout = strtod(argv[i] + strlen("--timeout="), nullptr);
		else if (!strncmp(argv[i], "--max-mse=", strlen("--max-mse=")) && strlen(argv[i]) > strlen("--max-mse="))
			testOptions.maxScreenshotError = strtod(argv[i] + strlen("--max-mse="), nullptr);
		else if (!strncmp(argv[i], "--debugger=", strlen("--debugger=")) && strlen(argv[i]) > strlen("--debugger="))
			debuggerPort = (int)strtoul(argv[i] + strlen("--debugger="), NULL, 10);
		else if (!strcmp(argv[i], "--teamcity"))
			teamCityMode = true;
		else if (!strncmp(argv[i], "--state=", strlen("--state=")) && strlen(argv[i]) > strlen("--state="))
			stateToLoad = argv[i] + strlen("--state=");
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
			return printUsage(argv[0], NULL);
		else
			testFilenames.push_back(argv[i]);
	}

	// TODO: Allow a filename here?
	if (testFilenames.size() == 1 && testFilenames[0] == "@-")
	{
		testFilenames.clear();
		char temp[2048];
		temp[2047] = '\0';

		while (scanf("%2047s", temp) == 1)
			testFilenames.push_back(temp);
	}

	if (testFilenames.empty())
		return printUsage(argv[0], argc <= 1 ? NULL : "No executables specified");

	LogManager::Init(&g_Config.bEnableLogging);
	LogManager *logman = LogManager::GetInstance();

	PrintfLogger *printfLogger = new PrintfLogger();

	for (int i = 0; i < LogTypes::NUMBER_OF_LOGS; i++) {
		LogTypes::LOG_TYPE type = (LogTypes::LOG_TYPE)i;
		logman->SetEnabled(type, fullLog);
		logman->SetLogLevel(type, LogTypes::LDEBUG);
	}
	logman->AddListener(printfLogger);

	// Needs to be after log so we don't interfere with test output.
	g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);

	HeadlessHost *headlessHost = getHost(gpuCore);
	headlessHost->SetGraphicsCore(gpuCore);
	host = headlessHost;

	std::string error_string;
	GraphicsContext *graphicsContext = nullptr;
	bool glWorking = host->InitGraphics(&error_string, &graphicsContext);

	CoreParameter coreParameter;
	coreParameter.cpuCore = cpuCore;
	coreParameter.gpuCore = glWorking ? gpuCore : GPUCORE_SOFTWARE;
	coreParameter.graphicsContext = graphicsContext;
	coreParameter.enableSound = false;
	coreParameter.mountIso = mountIso ? Path(std::string(mountIso)) : Path();
	coreParameter.mountRoot = mountRoot ? Path(std::string(mountRoot)) : Path();
	coreParameter.startBreak = false;
	coreParameter.printfEmuLog = !testOptions.compare;
	coreParameter.headLess = true;
	coreParameter.renderScaleFactor = 1;
	coreParameter.renderWidth = 480;
	coreParameter.renderHeight = 272;
	coreParameter.pixelWidth = 480;
	coreParameter.pixelHeight = 272;
	coreParameter.fastForward = true;

	g_Config.bEnableSound = false;
	g_Config.bFirstRun = false;
	g_Config.bIgnoreBadMemAccess = true;
	// Never report from tests.
	g_Config.sReportHost.clear();
	g_Config.bAutoSaveSymbolMap = false;
	g_Config.bSkipBufferEffects = false;
	g_Config.bSkipGPUReadbacks = false;
	g_Config.bHardwareTransform = true;
	g_Config.iAnisotropyLevel = 0;  // When testing mipmapping we really don't want this.
	g_Config.iMultiSampleLevel = 0;
	g_Config.bVertexCache = false;
	g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	g_Config.iTimeFormat = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
	g_Config.bEncryptSave = true;
	g_Config.sNickName = "shadow";
	g_Config.iTimeZone = 60;
	g_Config.iDateFormat = PSP_SYSTEMPARAM_DATE_FORMAT_DDMMYYYY;
	g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;
	g_Config.iLockParentalLevel = 9;
	g_Config.iInternalResolution = 1;
	g_Config.iFastForwardMode = (int)FastForwardMode::CONTINUOUS;
	g_Config.bEnableLogging = fullLog;
	g_Config.bSoftwareSkinning = true;
	g_Config.bVertexDecoderJit = true;
	g_Config.bSoftwareRendering = coreParameter.gpuCore == GPUCORE_SOFTWARE;
	g_Config.bSoftwareRenderingJit = true;
	g_Config.iSplineBezierQuality = 2;
	g_Config.bHighQualityDepth = true;
	g_Config.bMemStickInserted = true;
	g_Config.iMemStickSizeGB = 16;
	g_Config.bEnableWlan = true;
	g_Config.sMACAddress = "12:34:56:78:9A:BC";
	g_Config.iFirmwareVersion = PSP_DEFAULT_FIRMWARE;
	g_Config.iPSPModel = PSP_MODEL_SLIM;
	g_Config.iGlobalVolume = VOLUME_FULL;
	g_Config.iReverbVolume = VOLUME_FULL;

#if PPSSPP_PLATFORM(WINDOWS)
	g_Config.internalDataDirectory.clear();
	InitSysDirectories();
#endif

#if !PPSSPP_PLATFORM(ANDROID) && !PPSSPP_PLATFORM(WINDOWS)
	g_Config.memStickDirectory = Path(std::string(getenv("HOME"))) / ".ppsspp";
	g_Config.flash0Directory = File::GetExeDirectory() / "assets/flash0";
#endif

	// Try to find the flash0 directory.  Often this is from a subdirectory.
	for (int i = 0; i < 4 && !File::Exists(g_Config.flash0Directory); ++i) {
		if (File::Exists(g_Config.flash0Directory / ".." / "assets/flash0"))
			g_Config.flash0Directory = g_Config.flash0Directory / ".." / "assets/flash0";
		else
			g_Config.flash0Directory = g_Config.flash0Directory / ".." / ".." / "flash0";
	}
	// Or else, maybe in the executable's dir.
	if (!File::Exists(g_Config.flash0Directory))
		g_Config.flash0Directory = File::GetExeDirectory() / "assets/flash0";

	if (screenshotFilename)
		headlessHost->SetComparisonScreenshot(Path(std::string(screenshotFilename)), testOptions.maxScreenshotError);
	headlessHost->SetWriteFailureScreenshot(!teamCityMode && !getenv("GITHUB_ACTIONS") && !testOptions.bench);

#if PPSSPP_PLATFORM(ANDROID)
	// For some reason the debugger installs it with this name?
	if (File::Exists(Path("/data/app/org.ppsspp.ppsspp-2.apk"))) {
		g_VFS.Register("", ZipFileReader::Create(Path("/data/app/org.ppsspp.ppsspp-2.apk"), "assets/"));
	}
	if (File::Exists(Path("/data/app/org.ppsspp.ppsspp.apk"))) {
		g_VFS.Register("", ZipFileReader::Create(Path("/data/app/org.ppsspp.ppsspp.apk"), "assets/"));
	}
#elif !PPSSPP_PLATFORM(WINDOWS)
	g_VFS.Register("", new DirectoryReader(g_Config.flash0Directory / ".."));
#endif

	UpdateUIState(UISTATE_INGAME);

	if (debuggerPort > 0) {
		g_Config.iRemoteISOPort = debuggerPort;
		coreParameter.startBreak = true;
		StartWebServer(WebServerFlags::DEBUGGER);
	}

	if (stateToLoad != NULL)
		SaveState::Load(Path(stateToLoad), -1);

	std::vector<std::string> failedTests;
	std::vector<std::string> passedTests;
	for (size_t i = 0; i < testFilenames.size(); ++i)
	{
		coreParameter.fileToStart = Path(testFilenames[i]);
		if (testOptions.compare)
			printf("%s:\n", coreParameter.fileToStart.c_str());
		bool passed = RunAutoTest(headlessHost, coreParameter, testOptions);
		if (testOptions.bench) {
			double st = time_now_d();
			double deadline = st + testOptions.timeout;
			double runs = 0.0;
			for (int i = 0; i < 100; ++i) {
				RunAutoTest(headlessHost, coreParameter, testOptions);
				runs++;

				if (time_now_d() > deadline)
					break;
			}
			double et = time_now_d();

			std::string testName = GetTestName(coreParameter.fileToStart);
			printf("  %s - %f seconds average\n", testName.c_str(), (et - st) / runs);
		}
		if (testOptions.compare) {
			std::string testName = GetTestName(coreParameter.fileToStart);
			if (passed) {
				passedTests.push_back(testName);
				printf("  %s - passed!\n", testName.c_str());
			}
			else
				failedTests.push_back(testName);
		}
	}

	if (testOptions.compare) {
		printf("%d tests passed, %d tests failed.\n", (int)passedTests.size(), (int)failedTests.size());
		if (!failedTests.empty())
		{
			printf("Failed tests:\n");
			for (size_t i = 0; i < failedTests.size(); ++i) {
				printf("  %s\n", failedTests[i].c_str());
			}
		}
	}

	if (debuggerPort > 0) {
		ShutdownWebServer();
	}

	host->ShutdownGraphics();
	delete host;
	host = nullptr;
	headlessHost = nullptr;

	g_VFS.Clear();
	LogManager::Shutdown();
	delete printfLogger;

#if PPSSPP_PLATFORM(WINDOWS)
	timeEndPeriod(1);
#endif

	g_threadManager.Teardown();

	if (!failedTests.empty() && !teamCityMode)
		return 1;
	return 0;
}
