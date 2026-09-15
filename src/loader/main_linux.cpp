#include "ini.h"
#ifdef REVLOADER_STANDALONE
#include "launcher.h"
#endif

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

/* Vellum loader for native Linux GoldSrc. SteamAPI_Init dlopens
 * $HOME/.steam/sdk32/steamclient.so by absolute path (not LD_LIBRARY_PATH),
 * so we point HOME at a private .vellum-home that symlinks our steamclient.
 * LD_LIBRARY_PATH still has the game dir plus Steam Runtime i386 libs (GTK2/CEF).
 * Loader mode execs ProcName (default ./hl from raspad-hl). */

#define DEFAULT_STEAM_APPID "10"
#define DEFAULT_PROC        "./hl"

static void Fail(const char *text)
{
	fprintf(stderr, "vellum: %s\n", text);
}

static void JoinPath(char *out, size_t outSize, const char *dir, const char *file)
{
	size_t n = strlen(dir);
	if (n > 0 && dir[n - 1] == '/') {
		snprintf(out, outSize, "%s%s", dir, file);
	} else {
		snprintf(out, outSize, "%s/%s", dir, file);
	}
	out[outSize - 1] = '\0';
}

static void StripTrailingSlash(char *dir)
{
	size_t n = strlen(dir);
	while (n > 1 && dir[n - 1] == '/') {
		dir[--n] = '\0';
	}
}

static void DirFromSelf(char *dir, size_t dirSize)
{
	ssize_t n = readlink("/proc/self/exe", dir, dirSize - 1);
	char *slash;

	if (n <= 0) {
		if (getcwd(dir, dirSize) == NULL) {
			strncpy(dir, ".", dirSize - 1);
			dir[dirSize - 1] = '\0';
		}
		return;
	}
	dir[n] = '\0';
	slash = strrchr(dir, '/');
	if (slash != NULL) {
		if (slash == dir) {
			slash[1] = '\0';
		} else {
			slash[1] = '\0';
		}
	}
}

static void AppendArg(char *cmd, size_t cmdSize, const char *arg)
{
	size_t n = strlen(cmd);
	if (n > 0 && n + 1 < cmdSize) {
		cmd[n++] = ' ';
		cmd[n] = '\0';
	}
	snprintf(cmd + n, cmdSize - n, "%s", arg);
	cmd[cmdSize - 1] = '\0';
}

#ifdef REVLOADER_LAUNCHER_DLLS
static const char *SkipSpaces(const char *p)
{
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return p;
}

static const char *NextToken(const char *p, char *out, size_t outSize)
{
	size_t n = 0;

	p = SkipSpaces(p);
	if (*p == '\0') {
		out[0] = '\0';
		return p;
	}
	if (*p == '"') {
		p++;
		while (*p != '\0' && *p != '"' && n + 1 < outSize) {
			out[n++] = *p++;
		}
		if (*p == '"') {
			p++;
		}
	} else {
		while (*p != '\0' && *p != ' ' && *p != '\t' && n + 1 < outSize) {
			out[n++] = *p++;
		}
	}
	out[n] = '\0';
	return p;
}

static int SoNameIsSafe(const char *name)
{
	size_t len;
	const char *p;

	if (name == NULL || name[0] == '\0') {
		return 0;
	}
	for (p = name; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '"' || *p == '\'') {
			return 0;
		}
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		return 0;
	}
	if (strstr(name, "..") != NULL) {
		return 0;
	}
	len = strlen(name);
	if (len < 4 || strcmp(name + len - 3, ".so") != 0) {
		return 0;
	}
	return 1;
}

static int CmdlineHasDll(const char *cmd, const char *name)
{
	const char *p = cmd;
	char tok[512];
	char got[512];

	while (*p != '\0') {
		p = NextToken(p, tok, sizeof(tok));
		if (tok[0] == '\0') {
			break;
		}
		if (strcmp(tok, "-dll") != 0) {
			continue;
		}
		p = NextToken(p, got, sizeof(got));
		if (strcmp(got, name) == 0) {
			return 1;
		}
	}
	return 0;
}

static int AppendDllsFromIni(char *cmd, size_t cmdSize, const char *iniPath)
{
	char list[1024];
	char name[512];
	const char *p;
	size_t n;

	Vellum_IniGet(iniPath, "Loader", "Dlls", list, sizeof(list), "");
	p = list;
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		n = 0;
		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' && n + 1 < sizeof(name)) {
			name[n++] = *p++;
		}
		name[n] = '\0';
		if (!SoNameIsSafe(name)) {
			Fail("Invalid Dlls entry in rev.ini (basename only, .so in the game folder).");
			return 0;
		}
		if (CmdlineHasDll(cmd, name)) {
			continue;
		}
		AppendArg(cmd, cmdSize, "-dll");
		AppendArg(cmd, cmdSize, name);
	}
	return 1;
}
#endif

static int HasArg(const char *cmd, const char *arg)
{
	const char *p = cmd;
	size_t n = strlen(arg);

	while ((p = strstr(p, arg)) != NULL) {
		if (p == cmd || p[-1] == ' ' || p[-1] == '\t') {
			char end = p[n];
			if (end == '\0' || end == ' ' || end == '\t') {
				return 1;
			}
		}
		p += n;
	}
	return 0;
}

#ifdef REVLOADER_STANDALONE
static void AppendLaunchTail(char *cmd, size_t cmdSize, const char *procName)
{
	const char *rest = procName;

	if (rest[0] == '"') {
		rest = strchr(rest + 1, '"');
		if (rest == NULL) {
			return;
		}
		rest++;
	} else {
		while (*rest != '\0' && *rest != ' ' && *rest != '\t') {
			rest++;
		}
	}
	while (*rest == ' ' || *rest == '\t') {
		rest++;
	}
	if (*rest != '\0') {
		AppendArg(cmd, cmdSize, rest);
	}
}

static int ArgvHasToken(int argc, char **argv, const char *arg)
{
	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], arg) == 0) {
			return 1;
		}
	}
	return 0;
}

static void JoinArgv(char *cmd, size_t cmdSize, int argc, char **argv)
{
	int i;
	cmd[0] = '\0';
	for (i = 0; i < argc; i++) {
		AppendArg(cmd, cmdSize, argv[i]);
	}
}
#endif

static int ReadSteamAppId(const char *dir, char *out, size_t outSize)
{
	char path[4096];
	FILE *f;
	size_t n;

	JoinPath(path, sizeof(path), dir, "steam_appid.txt");
	f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	n = fread(out, 1, outSize - 1, f);
	fclose(f);
	out[n] = '\0';
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
		out[--n] = '\0';
	}
	return n > 0;
}

static void WriteSteamAppId(const char *dir, const char *appId)
{
	char path[4096];
	FILE *f;

	if (appId == NULL || appId[0] == '\0') {
		return;
	}
	JoinPath(path, sizeof(path), dir, "steam_appid.txt");
	f = fopen(path, "w");
	if (f == NULL) {
		return;
	}
	fprintf(f, "%s\n", appId);
	fclose(f);
}

static int FileReadable(const char *path)
{
	return access(path, R_OK) == 0;
}

static int EnsureDir(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST) {
		return 1;
	}
	return 0;
}

static int LinkOver(const char *target, const char *linkPath)
{
	unlink(linkPath);
	if (symlink(target, linkPath) != 0) {
		return 0;
	}
	return 1;
}

/* GoldSrc calls setlocale("en_US.UTF-8"). Debian often only has C.utf8 / ru_RU.utf8. */
static int LocaleDirReady(const char *path)
{
	char marker[4096];
	JoinPath(marker, sizeof(marker), path, "LC_CTYPE");
	return access(marker, R_OK) == 0;
}

static void EnsureEnUsUtf8(const char *vellumHome)
{
	char locRoot[4096];
	char locDir[4096];
	pid_t pid;
	int status;

	if (setlocale(LC_ALL, "en_US.UTF-8") != NULL) {
		setenv("LANG", "en_US.UTF-8", 1);
		setenv("LC_ALL", "en_US.UTF-8", 1);
		return;
	}

	JoinPath(locRoot, sizeof(locRoot), vellumHome, "locale");
	if (!EnsureDir(locRoot)) {
		return;
	}
	JoinPath(locDir, sizeof(locDir), locRoot, "en_US.UTF-8");
	if (!LocaleDirReady(locDir)) {
		pid = fork();
		if (pid == 0) {
			execlp("localedef", "localedef", "-c", "-f", "UTF-8", "-i", "en_US",
			       "--no-archive", locDir, (char *)NULL);
			_exit(127);
		}
		if (pid > 0) {
			waitpid(pid, &status, 0);
		}
	}
	if (LocaleDirReady(locDir)) {
		setenv("LOCPATH", locRoot, 1);
		setenv("LANG", "en_US.UTF-8", 1);
		setenv("LC_ALL", "en_US.UTF-8", 1);
	}
}

static void AppendDirIfExists(char *val, size_t valSize, const char *path)
{
	size_t n;

	if (path == NULL || path[0] == '\0' || access(path, R_OK) != 0) {
		return;
	}
	n = strlen(val);
	if (n > 0 && n + 1 < valSize) {
		val[n++] = ':';
		val[n] = '\0';
	}
	snprintf(val + n, valSize - n, "%s", path);
	val[valSize - 1] = '\0';
}

static void AppendSteamRuntimeI386(char *val, size_t valSize, const char *realHome)
{
	static const char *kRel[] = {
		".local/share/Steam/ubuntu12_32/steam-runtime/pinned_libs_32",
		".local/share/Steam/ubuntu12_32/steam-runtime/lib/i386-linux-gnu",
		".local/share/Steam/ubuntu12_32/steam-runtime/usr/lib/i386-linux-gnu",
		".steam/steam/ubuntu12_32/steam-runtime/pinned_libs_32",
		".steam/steam/ubuntu12_32/steam-runtime/lib/i386-linux-gnu",
		".steam/steam/ubuntu12_32/steam-runtime/usr/lib/i386-linux-gnu",
		NULL
	};
	char path[4096];
	int i;

	if (realHome == NULL || realHome[0] == '\0') {
		return;
	}
	for (i = 0; kRel[i] != NULL; i++) {
		snprintf(path, sizeof(path), "%s/%s", realHome, kRel[i]);
		path[sizeof(path) - 1] = '\0';
		AppendDirIfExists(val, valSize, path);
	}
}

static void KeepRealHomeXdg(const char *realHome)
{
	char path[4096];

	if (realHome == NULL || realHome[0] == '\0') {
		return;
	}
	if (getenv("XDG_DATA_HOME") == NULL) {
		snprintf(path, sizeof(path), "%s/.local/share", realHome);
		path[sizeof(path) - 1] = '\0';
		setenv("XDG_DATA_HOME", path, 0);
	}
	if (getenv("XDG_CONFIG_HOME") == NULL) {
		snprintf(path, sizeof(path), "%s/.config", realHome);
		path[sizeof(path) - 1] = '\0';
		setenv("XDG_CONFIG_HOME", path, 0);
	}
	if (getenv("XDG_CACHE_HOME") == NULL) {
		snprintf(path, sizeof(path), "%s/.cache", realHome);
		path[sizeof(path) - 1] = '\0';
		setenv("XDG_CACHE_HOME", path, 0);
	}
}

/* libsteam_api dlopens $HOME/.steam/sdk32/steamclient.so by absolute path.
 * Point HOME at a private tree so that is our steamclient, not Steam's. */
static int BindVellumSteamHome(const char *gameDir, const char *steamClient, char *homeOut, size_t homeSize)
{
	char steamDir[4096];
	char sdk32[4096];
	char bin32[4096];
	char steamRoot[4096];
	char linux32[4096];
	char linkPath[4096];
	char absClient[4096];

	if (steamClient[0] == '/') {
		strncpy(absClient, steamClient, sizeof(absClient) - 1);
		absClient[sizeof(absClient) - 1] = '\0';
	} else {
		JoinPath(absClient, sizeof(absClient), gameDir, steamClient);
	}

	JoinPath(homeOut, homeSize, gameDir, ".vellum-home");
	StripTrailingSlash(homeOut);
	if (!EnsureDir(homeOut)) {
		return 0;
	}
	JoinPath(steamDir, sizeof(steamDir), homeOut, ".steam");
	if (!EnsureDir(steamDir)) {
		return 0;
	}
	JoinPath(sdk32, sizeof(sdk32), steamDir, "sdk32");
	JoinPath(bin32, sizeof(bin32), steamDir, "bin32");
	JoinPath(steamRoot, sizeof(steamRoot), steamDir, "steam");
	JoinPath(linux32, sizeof(linux32), steamRoot, "linux32");
	if (!EnsureDir(sdk32) || !EnsureDir(bin32) || !EnsureDir(steamRoot) || !EnsureDir(linux32)) {
		return 0;
	}
	JoinPath(linkPath, sizeof(linkPath), sdk32, "steamclient.so");
	if (!LinkOver(absClient, linkPath)) {
		return 0;
	}
	JoinPath(linkPath, sizeof(linkPath), bin32, "steamclient.so");
	if (!LinkOver(absClient, linkPath)) {
		return 0;
	}
	JoinPath(linkPath, sizeof(linkPath), linux32, "steamclient.so");
	if (!LinkOver(absClient, linkPath)) {
		return 0;
	}
	return 1;
}

static void PrependLdLibraryPath(const char *dir, const char *realHome)
{
	char clean[4096];
	char val[8192];
	const char *old;
	size_t n;

	strncpy(clean, dir, sizeof(clean) - 1);
	clean[sizeof(clean) - 1] = '\0';
	n = strlen(clean);
	while (n > 1 && clean[n - 1] == '/') {
		clean[--n] = '\0';
	}

	old = getenv("LD_LIBRARY_PATH");
	if (old != NULL && old[0] != '\0') {
		snprintf(val, sizeof(val), "%s:%s", clean, old);
	} else {
		snprintf(val, sizeof(val), "%s", clean);
	}
	val[sizeof(val) - 1] = '\0';
	AppendSteamRuntimeI386(val, sizeof(val), realHome);
	setenv("LD_LIBRARY_PATH", val, 1);
}

static pid_t ReadPidFile(const char *path)
{
	FILE *f;
	long pid = 0;

	f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	if (fscanf(f, "%ld", &pid) != 1) {
		fclose(f);
		return 0;
	}
	fclose(f);
	if (pid <= 0) {
		return 0;
	}
	return (pid_t)pid;
}

static int PidIsLive(pid_t pid)
{
	if (pid <= 0) {
		return 0;
	}
	return kill(pid, 0) == 0 || errno == EPERM;
}

/* steam_api.so: SteamAPI_IsSteamRunning reads ~/.steam/steam.pid (and
 * ~/.steampid). If a live pid is already there (real Steam), leave it.
 * Otherwise this process is the stand-in — exec keeps the same pid. */
static void EnsureSteamLooksRunning(void)
{
	const char *home = getenv("HOME");
	char steamDir[4096];
	char pidPath[4096];
	char altPath[4096];
	pid_t pid;
	FILE *f;

	if (home == NULL || home[0] == '\0') {
		return;
	}

	snprintf(steamDir, sizeof(steamDir), "%s/.steam", home);
	steamDir[sizeof(steamDir) - 1] = '\0';
	snprintf(pidPath, sizeof(pidPath), "%s/steam.pid", steamDir);
	pidPath[sizeof(pidPath) - 1] = '\0';
	snprintf(altPath, sizeof(altPath), "%s/.steampid", home);
	altPath[sizeof(altPath) - 1] = '\0';

	pid = ReadPidFile(pidPath);
	if (PidIsLive(pid)) {
		return;
	}
	pid = ReadPidFile(altPath);
	if (PidIsLive(pid)) {
		return;
	}

	mkdir(steamDir, 0755);

	f = fopen(pidPath, "w");
	if (f != NULL) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	char dir[4096];
	char iniPath[4096];
	char procName[1024];
	char extraArgs[1024];
	char appId[256];
	char steamClient[4096];
	char iniClient[256];
	int i;

	DirFromSelf(dir, sizeof(dir));
	if (chdir(dir) != 0) {
		Fail("Unable to chdir to the executable directory.");
		return 1;
	}
	JoinPath(iniPath, sizeof(iniPath), dir, "rev.ini");

	procName[0] = '\0';
	extraArgs[0] = '\0';
	appId[0] = '\0';
	steamClient[0] = '\0';

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-launch") == 0 && i + 1 < argc) {
			strncpy(procName, argv[++i], sizeof(procName) - 1);
			procName[sizeof(procName) - 1] = '\0';
		} else if (strcmp(argv[i], "-appid") == 0 && i + 1 < argc) {
			strncpy(appId, argv[++i], sizeof(appId) - 1);
			appId[sizeof(appId) - 1] = '\0';
		} else {
			AppendArg(extraArgs, sizeof(extraArgs), argv[i]);
		}
	}

	if (procName[0] == '\0') {
		Vellum_IniGet(iniPath, "Loader", "ProcName", procName, sizeof(procName), "");
		if (procName[0] == '\0') {
			strncpy(procName, DEFAULT_PROC, sizeof(procName) - 1);
			procName[sizeof(procName) - 1] = '\0';
		}
	}

#ifndef REVLOADER_STANDALONE
	if (extraArgs[0] != '\0') {
		AppendArg(procName, sizeof(procName), extraArgs);
	}

	if (!HasArg(procName, "-game")) {
		AppendArg(procName, sizeof(procName), "-game");
		AppendArg(procName, sizeof(procName), "cstrike");
	}

#ifdef REVLOADER_LAUNCHER_DLLS
	if (!AppendDllsFromIni(procName, sizeof(procName), iniPath)) {
		return 1;
	}
#endif
#endif

	if (appId[0] == '\0' && !ReadSteamAppId(dir, appId, sizeof(appId))) {
		strncpy(appId, DEFAULT_STEAM_APPID, sizeof(appId) - 1);
		appId[sizeof(appId) - 1] = '\0';
	}

	setenv("SteamGameId", appId, 1);
	setenv("SteamAppId", appId, 1);
	setenv("SteamEnv", "1", 1);
	{
		char steamPath[4096];
		strncpy(steamPath, dir, sizeof(steamPath) - 1);
		steamPath[sizeof(steamPath) - 1] = '\0';
		StripTrailingSlash(steamPath);
		setenv("SteamPath", steamPath, 1);
	}
	WriteSteamAppId(dir, appId);

	iniClient[0] = '\0';
	Vellum_IniGet(iniPath, "Loader", "SteamClientDll", iniClient, sizeof(iniClient), "");
	if (iniClient[0] != '\0') {
		if (strchr(iniClient, '/') != NULL) {
			strncpy(steamClient, iniClient, sizeof(steamClient) - 1);
			steamClient[sizeof(steamClient) - 1] = '\0';
		} else {
			JoinPath(steamClient, sizeof(steamClient), dir, iniClient);
		}
	} else {
		JoinPath(steamClient, sizeof(steamClient), dir, "steamclient.so");
	}

	if (!FileReadable(steamClient)) {
		char msg[512];
		snprintf(msg, sizeof(msg), "Can't find steamclient.so relative to executable path %s", dir);
		Fail(msg);
		return 1;
	}

	{
		const char *realHome = getenv("HOME");
		char vellumHome[4096];
		char realHomeBuf[4096];

		realHomeBuf[0] = '\0';
		if (realHome != NULL && realHome[0] != '\0') {
			strncpy(realHomeBuf, realHome, sizeof(realHomeBuf) - 1);
			realHomeBuf[sizeof(realHomeBuf) - 1] = '\0';
		}
		KeepRealHomeXdg(realHomeBuf);
		if (!BindVellumSteamHome(dir, steamClient, vellumHome, sizeof(vellumHome))) {
			Fail("Unable to bind a private $HOME/.steam for steamclient.so.");
			return 1;
		}
		setenv("HOME", vellumHome, 1);
		EnsureEnUsUtf8(vellumHome);
		PrependLdLibraryPath(dir, realHomeBuf);
	}
	EnsureSteamLooksRunning();

#ifdef REVLOADER_STANDALONE
	{
		char engineCmd[4096];

		JoinArgv(engineCmd, sizeof(engineCmd), argc, argv);
		if (procName[0] != '\0' && !HasArg(engineCmd, "-game")) {
			AppendLaunchTail(engineCmd, sizeof(engineCmd), procName);
		}
		if (!HasArg(engineCmd, "-game")) {
			AppendArg(engineCmd, sizeof(engineCmd), "-game");
			AppendArg(engineCmd, sizeof(engineCmd), "cstrike");
		}
#ifdef REVLOADER_LAUNCHER_DLLS
		if (!AppendDllsFromIni(engineCmd, sizeof(engineCmd), iniPath)) {
			return 1;
		}
#endif
		if (!ArgvHasToken(argc, argv, "-game")) {
			char selfPath[4096];
			char *nargv[64];
			int n = 0;
			int j;
			ssize_t rn = readlink("/proc/self/exe", selfPath, sizeof(selfPath) - 1);

			if (rn <= 0) {
				Fail("Unable to re-exec with -game.");
				return 1;
			}
			selfPath[rn] = '\0';
			nargv[n++] = selfPath;
			for (j = 1; j < argc && n + 3 < 64; j++) {
				nargv[n++] = argv[j];
			}
			nargv[n++] = (char *)"-game";
			nargv[n++] = (char *)"cstrike";
			nargv[n] = NULL;
			execv(selfPath, nargv);
			Fail("Unable to re-exec with -game.");
			return 1;
		}
		return HlLauncher_Run(NULL, engineCmd);
	}
#else
	{
		char shellCmd[4096];
		snprintf(shellCmd, sizeof(shellCmd), "exec %s", procName);
		shellCmd[sizeof(shellCmd) - 1] = '\0';
		execl("/bin/sh", "sh", "-c", shellCmd, (char *)NULL);
		{
			char msg[512];
			snprintf(msg, sizeof(msg), "Unable to execute command %s (%s)", procName, strerror(errno));
			Fail(msg);
		}
		return 1;
	}
#endif
}
