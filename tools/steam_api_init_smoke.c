#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
	const char *home;
	char pidpath[512];
	FILE *pf;
	void *api;
	int (*is_running)(void);
	int (*init)(void);
	int running;
	int ok;

	setenv("SteamAppId", "10", 1);
	setenv("SteamGameId", "10", 1);
	setenv("SteamEnv", "1", 1);

	home = getenv("HOME");
	if (home != NULL) {
		snprintf(pidpath, sizeof(pidpath), "%s/.steam", home);
		mkdir(pidpath, 0755);
		snprintf(pidpath, sizeof(pidpath), "%s/.steam/steam.pid", home);
		pf = fopen(pidpath, "w");
		if (pf != NULL) {
			fprintf(pf, "%d\n", (int)getpid());
			fclose(pf);
		}
	}

	api = dlopen("./libsteam_api.so", RTLD_NOW);
	if (api == NULL) {
		fprintf(stderr, "dlopen libsteam_api.so: %s\n", dlerror());
		return 1;
	}
	is_running = (int (*)(void))dlsym(api, "SteamAPI_IsSteamRunning");
	init = (int (*)(void))dlsym(api, "SteamAPI_Init");
	if (init == NULL) {
		fprintf(stderr, "no SteamAPI_Init: %s\n", dlerror());
		return 1;
	}
	running = is_running != NULL ? is_running() : -1;
	printf("SteamAPI_IsSteamRunning=%d\n", running);
	ok = init();
	printf("SteamAPI_Init=%d\n", ok);
	return ok ? 0 : 2;
}
