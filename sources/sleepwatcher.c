/*
 *	sleepwatcher
 *
 *	Copyright (c) 2002-2011 Bernhard Baehr
 *
 *	sleepwatcher.c - sleep mode watchdog program
 *
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/wait.h>

#include <mach/mach_port.h>
#include <mach/mach_interface.h>
#include <mach/mach_init.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>


#define TIMER_RESOLUTION	0.1		/* seconds - when changing, adjust the man page! */

#define DENY_SLEEP		((char *) 1)	/* for args.allowsleepcommand */


static struct args {
	int		argc;
	char * const    *argv;
	char		*progname;
	int		verbose;
	int		daemon;
	char		*pidfile;
	char		*allowsleepcommand;
	char		*cantsleepcommand;
	char		*sleepcommand;
	char		*wakeupcommand;
	char		*displaydimcommand;
	char		*displayundimcommand;
	char		*displaysleepcommand;
	char		*displaywakeupcommand;
	long int	idletimeout;
	char		*idlecommand;
	int		idleresume;		// not an option, but a flag to execute idleresumecommand
	char		*idleresumecommand;
	long int	breaklength;
	char		*resumecommand;
	char		*plugcommand;
	char		*unplugcommand;
}	args;


static const struct option longopts[] = {
	{ "now",		no_argument,		NULL, 'n' },
	{ "version",		no_argument,		NULL, 'v' },
	{ "verbose",		no_argument,		NULL, 'V' },
	{ "daemon",		no_argument,		NULL, 'd' },
	{ "getidletime",	no_argument,		NULL, 'g' },
	{ "config",		required_argument,	NULL, 'f' },
	{ "pidfile",		required_argument,	NULL, 'p' },
	{ "allowsleep",		optional_argument,	NULL, 'a' },
	{ "cantsleep",		required_argument,	NULL, 'c' },
	{ "sleep",		required_argument,	NULL, 's' },
	{ "wakeup",		required_argument,	NULL, 'w' },
	{ "displaydim",		required_argument,	NULL, 'D' },
	{ "displayundim",	required_argument,	NULL, 'E' },
	{ "displaysleep",	required_argument,	NULL, 'S' },
	{ "displaywakeup",	required_argument,	NULL, 'W' },
	{ "timeout",		required_argument,	NULL, 't' },
	{ "idleresume",		required_argument,	NULL, 'R' },	// longer option name first
	{ "idle",		required_argument,	NULL, 'i' },
	{ "break",		required_argument,	NULL, 'b' },
	{ "resume",		required_argument,	NULL, 'r' },
	{ "plug",		required_argument,	NULL, 'P' },
	{ "unplug",		required_argument,	NULL, 'U' },
	{ NULL, 0, NULL, 0 }
};

#define GETOPT_STRING   "nvVdgf:p:a::c:s:w:D:E:S:W:t:R:i:b:r:P:U:"


static char *setstr (char *oldstr, char *newstr)
{
	if (oldstr)
		free (oldstr);
	return (newstr ? strdup(newstr) : NULL);
}


void message (int priority, const char *msg, ...)
{
	va_list ap;
	FILE    *out;
	
	if (args.verbose || priority < LOG_INFO) {
		va_start (ap, msg);
		if (args.daemon) {
			openlog (args.progname, LOG_PID, LOG_DAEMON);
			vsyslog (priority, msg, ap);
			closelog ();
		} else {
			out = (priority == LOG_INFO) ? stdout : stderr;
			fprintf (out, "%s: ", args.progname);
			vfprintf (out, msg, ap);
			fflush (out);
		}
	}
}


void writePidFile (char *pidfile)
{
	FILE    *fp;
	
	if (args.pidfile)
		unlink (args.pidfile);
	args.pidfile = setstr(args.pidfile, pidfile);
	if (args.pidfile) {
		fp = fopen(pidfile, "w");
		if (fp) {
			fprintf (fp, "%d", getpid());
			fclose (fp);
		} else
			message (LOG_ERR, "can't write pidfile %s\n", pidfile);
		
	}
}


static void usage (void)
{
        printf ("Usage: %s [-n] [-v] [-V] [-d] [-g] [-f configfile] [-p pidfile]\n"
		"		[-a[allowsleepcommand]] [-c cantsleepcommand]\n"
		"		[-s sleepcommand] [-w wakeupcommand]\n"
		"		[-D displaydimcommand] [-E displayundimcommand]\n"
		"		[-S displaysleepcommand] [-W displaywakeupcommand]\n"
		"		[-t timeout -i idlecommand [-R idleresumecommand]]\n"
		"		[-b break -r resumecommand]\n"
		"		[-P plugcommand] [-U unplugcommand]\n"
		"Daemon to monitor sleep, wakeup and idleness of the Mac\n"
		"-n or --now\n"
		"       sleep now and exit, ignoring other options\n"
		"-v or --version\n"
		"       display version and copyright information and exit\n"
		"-V or --verbose\n"
		"       log any action sleepwatcher performs\n"
		"-d or --daemon\n"
		"       run as a background daemon (don't use -d in conjunction with launchd)\n"
		"-g or --getidletime\n"
		"       print the time of no keyboard or mouse activity (in %g seconds)\n"
		"       and exit, ignoring other options\n"
		"-f or --config\n"
		"       read additional configuration parameters from configfile\n"
		"       (later, SIGHUP causes reconfiguration from a modified configfile)\n"
		"-p or --pidfile\n"
		"       write pidfile with the process id\n"
		"-a or --allowsleep\n"
		"       allow the Mac to sleep only when allowsleepcommand returns a zero\n"
		"       exit code, -a without allowsleepcommand denys sleeping\n"
		"       (note: no space between -a and the optional allowsleepcommand)\n"
		"-c or --cantsleep\n"
		"       execute cantsleepcommand when the Mac retracts an attempt to sleep that\n"
		"       that previously was allowed via the -a option, but vetoed by an other\n"
		"       process\n"
                "-s or --sleep\n"
		"       execute sleepcommand when the Mac is put to sleep mode\n"
		"       (sleepcommand must not take longer than 15 seconds because\n"
		"       after this timeout the sleep mode is forced by the system)\n"
		"-w or --wakeup\n"
		"       execute wakeupcommand when the Mac wakes up\n"
		"-D or --displaydim\n"
		"       execute displaydimcommand when the display of the Mac is dimmed\n"
		"-E or --displayundim\n"
		"       execute displayundimcommand when the display of the Mac is undimmed\n"
		"       (without having gone to sleep)\n"
		"-S or --displaysleep\n"
		"       execute displaysleepcommand when the display of the Mac is put to\n"
		"       sleep mode\n"
		"-W or --displaywakeup\n"
		"       execute displaywakeupcommand when the display of the Mac wakes up\n"
		"-t or --timeout\n"
		"       set timeout for the -i option (in %g seconds)\n"
		"-i or --idle\n"
		"       execute idlecommand when no user interaction (keyboard, mouse)\n"
		"       took place in the period given with the -t option\n"
		"-R or --idleresume\n"
		"       execute idleresumecommand when the user resumes mouse or keyboard\n"
		"       activity after the -i idlecommand was executed\n"
		"-b or --break\n"
		"       set length of a break for the -r option (in %g seconds)\n"
		"-r or --resume\n"
		"       execute resumecommand when the user resumes mouse or keyboard\n"
		"       activity after a break of a length specified with the -b option\n"
		"-P or --plug\n"
		"       execute plugcommand when a Mac notebook is connected to power supply\n"
		"-U or --unplug\n"
		"       execute unplugcommand when a Mac notebook is disconnected from\n"
		"       power supply\n",
		args.progname, TIMER_RESOLUTION, TIMER_RESOLUTION, TIMER_RESOLUTION);
	exit (2);
}


static void copyright (void)
{
	printf ("sleepwatcher 2.2\n"
		"Copyright (c) 2002-2011 Bernhard Baehr (bernhard.baehr@gmx.de)\n"
		"This is free software that comes with ABSOLUTELY NO WARRANTY.\n"
		"See the GNU General Public License for details.\n");
	exit (2);
}


static int sleepImmediately (void)
{
	mach_port_t	masterPort;
	io_connect_t	rootPort;
	IOReturn	err;

	if (IOPMSleepEnabled()) {
		if ((err = IOMasterPort(MACH_PORT_NULL, &masterPort))) {
			fprintf (stderr, "%s: can't get mach master port: %ld\n", args.progname, (long) err);
			return (1);
		}
		rootPort = IOPMFindPowerManagement(masterPort);
		if (rootPort) {
			err = IOPMSleepSystem(rootPort);
			if (err) {
				fprintf (stderr, "%s: IOPMSleepSystem failed: %ld\n", args.progname, (long) err);
				return (1);
			}
		} else {
			fprintf (stderr, "%s: IOPMFindPowerManagement failed\n", args.progname);
			return (1);
		}
	} else {
		fprintf (stderr, "%s: sleep mode is disabled\n", args.progname);
		return (1);
	}
	return (0);
}


static long int getIdleTime (void)
/* returns mouse and keyboard idle time in TIMER_RESOLUTION seconds; returns -1 on error */
{
	mach_port_t		masterPort = 0;
	io_iterator_t		iter = 0;
	io_registry_entry_t     curObj = 0;
	CFMutableDictionaryRef  properties = NULL;
	CFTypeRef		obj = NULL;
	CFTypeID		type = 0;
	uint64_t		idletime = -1;

	if (IOMasterPort (MACH_PORT_NULL, &masterPort) != KERN_SUCCESS) {
		message (LOG_ERR, "can't get IOMasterPort\n");
		goto error;
	}
	IOServiceGetMatchingServices (masterPort, IOServiceMatching(kIOHIDSystemClass), &iter);
	if (iter == 0) {
		message (LOG_ERR, "can't access IOHIDSystem\n");
		goto error;
	}
	curObj = IOIteratorNext(iter);
	if (curObj == 0) {
		message (LOG_ERR, "got empty IOIterator\n");
		goto error;
	}
	if (IORegistryEntryCreateCFProperties(curObj, &properties, kCFAllocatorDefault, 0) != KERN_SUCCESS || ! properties) {
		message (LOG_ERR, "can't access HIDIdleTime\n");
		goto error;
	}
	obj = CFDictionaryGetValue(properties, CFSTR(kIOHIDIdleTimeKey));
	CFRetain (obj);
	type = CFGetTypeID(obj);
	if (type == CFDataGetTypeID())
		CFDataGetBytes ((CFDataRef) obj, CFRangeMake(0, sizeof(idletime)), (UInt8 *) &idletime);   
	else if (type == CFNumberGetTypeID())
		CFNumberGetValue ((CFNumberRef) obj, kCFNumberSInt64Type, &idletime);
	else { 
		message (LOG_ERR, "unsupported idle time data type\n", (int) type);
		goto error;
	}
	idletime /= 1000000000l * TIMER_RESOLUTION;      /* transform from 10**-9 to TIMER_RESOLUTION seconds */
error :
	if (masterPort)
		mach_port_deallocate (mach_task_self(), masterPort);
	if (obj)
		CFRelease(obj);
	if (curObj)
		IOObjectRelease (curObj);
	if (iter)
		IOObjectRelease (iter);
	if (properties)
		CFRelease ((CFTypeRef) properties);
	return (idletime);
}


static void readConfig (const char *configfile);	/* forward declaration */


static long int scanTime (const char *arg, const char *msg)
{
	long int	t = 0;
	const char	*p = arg;
	
	while (*p && isdigit(*p))
		t = 10 * t + (*p++ - '0');
	if (*p) {
		message (LOG_ERR, msg, arg);
		t = -1;
	}
	return (t);
}


static void setOption (const char c, char *optarg)
{
	switch (c) {
	case 'n' :
		exit (sleepImmediately());
		break;
	case 'v' :
		copyright ();
		break;
	case 'V' :
		args.verbose = 1;
		break;
	case 'd' :
		args.daemon = 1;
		break;
	case 'g' :
		printf ("%ld\n", getIdleTime());
		exit (0);
		break;
	case 'f' :
		readConfig (optarg);
		break;
	case 'p' :
		writePidFile (optarg);
		break;
	case 'a' :
		if (args.allowsleepcommand == DENY_SLEEP)
			args.allowsleepcommand = NULL;
		args.allowsleepcommand = setstr(args.allowsleepcommand, optarg);
		if (args.allowsleepcommand == NULL)
			args.allowsleepcommand = DENY_SLEEP;
		break;
	case 'c' :
		args.cantsleepcommand = setstr(args.cantsleepcommand, optarg);
		break;
	case 's' :
		args.sleepcommand = setstr(args.sleepcommand, optarg);
		break;
	case 'w' :
		args.wakeupcommand = setstr(args.wakeupcommand, optarg);
		break;
	case 'D' :
		args.displaydimcommand = setstr(args.displaydimcommand, optarg);
		break;
	case 'E' :
		args.displayundimcommand = setstr(args.displayundimcommand, optarg);
		break;
	case 'S' :
		args.displaysleepcommand = setstr(args.displaysleepcommand, optarg);
		break;
	case 'W' :
		args.displaywakeupcommand = setstr(args.displaywakeupcommand, optarg);
		break;
	case 't' :
		args.idletimeout = scanTime(optarg, "invalid digit(s) in timeout argument '%s'\n");
		break;
	case 'i' :
		args.idlecommand = setstr(args.idlecommand, optarg);
		break;
	case 'R' :
		args.idleresume = 0;
		args.idleresumecommand = setstr(args.idleresumecommand, optarg);
		break;
	case 'b' :
		args.breaklength = scanTime(optarg, "invalid digit(s) in pause argument '%s'\n");
		break;
	case 'r' :
		args.resumecommand = setstr(args.resumecommand, optarg);
		break;
	case 'P' :
		args.plugcommand = setstr(args.plugcommand, optarg);
		break;
	case 'U' :
		args.unplugcommand = setstr(args.unplugcommand, optarg);
		break;
	default :
		exit (2);
		break;
	}
}


static void readConfig (const char *configfile)
{
	char		buf[1024], *p, *q;
	struct option   const *op;
	int		l;
	
	FILE *fp = fopen(configfile, "r");
	if (! fp) {
		message (LOG_ERR, "can't read config file %s\n", configfile);
		return;
	}
	while (fgets(buf, sizeof(buf), fp)) {
		if (*buf == '#' || *buf == ';')			/* ignore comment lines */
			continue;
		if ((p = strchr(buf, '\n')))			/* remove newline */
			*p = '\0';
		for (p = q = buf; *p && *p != '='; p++) {	/* remove blanks before '=' */
			if (! isblank(*p))
				*q++ = *p;
		}
		if (*p)
			*q++ = *p++;				/* '=' */
		while (*p && isblank(*p))			/* remove blanks before argument */
			p++;
		while (*q++ = *p++)				/* unmodified argument string */
			;
		for (op = longopts; op->name; op++) {
			l = strlen(op->name);
			if (! strncmp(op->name, buf, l)) {
				if (op->has_arg == no_argument && buf[l] ||
					op->has_arg != no_argument && buf[l] != '=')
					message (LOG_ERR, "malformed parameter '%s' in config file %s\n", buf, configfile);
				setOption (op->val, buf + l + 1);
				break;
			}
		}
		if (! op->name)
			message (LOG_ERR, "unknown parameter '%s' in config file %s\n", buf, configfile);
	}
	fclose (fp);
}


static void checkTimeoutCommand (long int *timeout, char **command,
	const char *msgCommandWithoutTimeout, const char *msgTimeoutWithoutCommand)
{
	if (*timeout == -1) {		/* invalid timeout value was specified */
		*timeout = 0;
		*command = setstr(*command, NULL);
	}
	if (*timeout == 0 && *command) {
		message (LOG_ERR, msgCommandWithoutTimeout);
		*command = setstr(*command, NULL);
	}
	if (*timeout && *command == NULL) {
		message (LOG_ERR, msgTimeoutWithoutCommand);
		*timeout = 0;
	}
}


static void parseArgs (int argc, char * const *argv)
{
	int	c, i;

	args.argc = argc;
	args.argv = argv;
	args.progname = basename(*argv);
	args.verbose = 0;
	args.daemon = 0;
	args.allowsleepcommand = setstr(args.allowsleepcommand, NULL);
	args.cantsleepcommand = setstr(args.cantsleepcommand, NULL);
	args.sleepcommand = setstr(args.sleepcommand, NULL);
	args.wakeupcommand = setstr(args.wakeupcommand, NULL);
	args.displaydimcommand = setstr(args.displaydimcommand, NULL);
	args.displayundimcommand = setstr(args.displayundimcommand, NULL);
	args.displaysleepcommand = setstr(args.displaysleepcommand, NULL);
	args.displaywakeupcommand = setstr(args.displaywakeupcommand, NULL);
	args.idletimeout = 0;
	args.idlecommand = setstr(args.idlecommand, NULL);
	args.idleresume = 0;
	args.idleresumecommand = setstr(args.idleresumecommand, NULL);
	args.breaklength = 0;
	args.resumecommand = setstr(args.resumecommand, NULL);
	args.plugcommand = setstr(args.plugcommand, NULL);
	args.unplugcommand = setstr(args.unplugcommand, NULL);
	writePidFile (NULL);
	if (argc == 1)
		usage ();
	optreset = optind = 1;
	while ((c = getopt_long(argc, argv, GETOPT_STRING, longopts, &i)) != -1)
		setOption (c, optarg);
	if (argc != optind)
		message (LOG_ERR, "superfluous arguments ignored: \"%s ...\"\n", argv[optind]);
	checkTimeoutCommand (&args.idletimeout, &args.idlecommand,
		"idlecommand without timeout ignored\n", "timeout without idlecommand ignored\n");
	checkTimeoutCommand (&args.breaklength, &args.resumecommand,
		"resumecommand without break ignored\n", "break without resumecommand ignored\n");
	if (! args.idlecommand && args.idleresumecommand)
		message (LOG_ERR, "idleresumecommand without idlecommand ignored\n");
	if (! args.allowsleepcommand && ! args.cantsleepcommand && ! args.sleepcommand &&
		! args.wakeupcommand && ! args.displaydimcommand && ! args.displayundimcommand &&
		! args.displaysleepcommand && ! args.displaywakeupcommand && ! args.idlecommand &&
		! args.resumecommand && ! args.plugcommand && ! args.unplugcommand)
		message (LOG_ERR, "no useful options set\n");
}


static void idleCallback (CFRunLoopTimerRef timer, void *info)
{
/*
	fprintf (stderr, "idle for %ld * %g seconds (timeout: %ld * %g seconds)\n",
		getIdleTime(), TIMER_RESOLUTION, args.idletimeout, TIMER_RESOLUTION);
*/
	message (LOG_INFO, "idle: %s: %d\n", args.idlecommand, system(args.idlecommand));
	args.idleresume = 1;
}


static CFRunLoopTimerRef setupTimer (long int timeout, CFRunLoopTimerRef timer, CFRunLoopTimerCallBack callback)
{
	if (timeout) {
		if (timer)
			CFRunLoopTimerSetNextFireDate (timer, CFAbsoluteTimeGetCurrent() + timeout * TIMER_RESOLUTION);
		else {
			timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
				CFAbsoluteTimeGetCurrent() + timeout * TIMER_RESOLUTION,
				kCFAbsoluteTimeIntervalSince1904, 0, 0, callback, NULL);
			CFRunLoopAddTimer (CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
		}
	} else {
		if (timer) {
			CFRunLoopTimerInvalidate (timer);
			CFRelease (timer);
			timer = NULL;
		}
	}
	return (timer);
}


static void setupIdleTimer (void)
{
	static CFRunLoopTimerRef idleTimer = NULL;
	
	idleTimer = setupTimer(args.idletimeout, idleTimer, idleCallback);
}


// THIS CALLBACK IS NOT CALLED WHEN THE GUI IS NOT RUNNING
static void hidCallback (void *context, IOReturn result, void *sender, IOHIDValueRef value)
{
	static CFAbsoluteTime timeOfLastCall = 0;
	
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (timeOfLastCall == 0)
		timeOfLastCall = now;
	if (args.breaklength && (now - timeOfLastCall) >= args.breaklength * TIMER_RESOLUTION) {
/*
		fprintf (stderr, "resumed after %.2f seconds (break: %ld * %g seconds)\n",
			now - timeOfLastCall, args.breaklength, TIMER_RESOLUTION);
*/
		message (LOG_INFO, "resume: %s: %d\n", args.resumecommand, system(args.resumecommand));
	}
	if (args.idleresume && args.idleresumecommand) {
		args.idleresume = 0;
		message (LOG_INFO, "idleresume: %s: %d\n", args.idleresumecommand, system(args.idleresumecommand));
	}
	timeOfLastCall = now;
	setupIdleTimer ();
}


static CFMutableDictionaryRef createDeviceMatchingDictionary (UInt32 usagePage, UInt32 usage)	// see TN 2187
{
	CFMutableDictionaryRef result = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
	if (result) {
		CFNumberRef pageCFNumberRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usagePage);
		if (pageCFNumberRef) {
			CFDictionarySetValue (result, CFSTR(kIOHIDDeviceUsagePageKey), pageCFNumberRef);
			CFRelease (pageCFNumberRef);
			CFNumberRef usageCFNumberRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
			if (usageCFNumberRef) {
				CFDictionarySetValue (result, CFSTR(kIOHIDDeviceUsageKey), usageCFNumberRef);
				CFRelease( usageCFNumberRef );
			} else {
				message (LOG_ERR, "CFNumberCreate failed for usage\n");
				exit (1);
			}
		} else {
			message (LOG_ERR, "CFNumberCreate failed for usagePage\n");
			exit (1);
		}
	} else {
		message (LOG_ERR, "CFDictionaryCreateMutable failed\n");
		exit (1);
	}
	return result;
}


static CFArrayRef createGenericDesktopMatchingDictionaries (void)	// see TN 2187
{
	CFMutableArrayRef matchingCFArrayRef = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
	if (matchingCFArrayRef) {
		CFDictionaryRef matchingCFDictRef = createDeviceMatchingDictionary(kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse);
		if (matchingCFDictRef) {
			CFArrayAppendValue (matchingCFArrayRef, matchingCFDictRef);
			CFRelease (matchingCFDictRef);
		} else {
			message (LOG_ERR, "createDeviceMatchingDictionary failed for mouse\n");
			exit (1);
		}
		matchingCFDictRef = createDeviceMatchingDictionary(kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard);
		if (matchingCFDictRef) {
			CFArrayAppendValue (matchingCFArrayRef, matchingCFDictRef);
			CFRelease (matchingCFDictRef);
		} else {
			message (LOG_ERR, "createDeviceMatchingDictionary failed for keyboard\n");
			exit (1);
		}
	} else {
		message (LOG_ERR, "CFArrayCreateMutable failed\n");
		exit (1);
	}
	return matchingCFArrayRef;
}


static void initializeResumeNotifications (void)	// see TN 2187
{
	IOHIDManagerRef hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	if (! hidManager) {
		message (LOG_ERR, "IOHIDManagerCreate failed\n");
		exit (1);
	}
	if (IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
		message (LOG_ERR, "IOHIDManagerOpen failed\n");
		exit (1);
	}
	IOHIDManagerScheduleWithRunLoop (hidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	IOHIDManagerSetDeviceMatchingMultiple (hidManager, createGenericDesktopMatchingDictionaries());
	IOHIDManagerRegisterInputValueCallback (hidManager, hidCallback, (void *) -1);
}


void powerCallback (void *rootPort, io_service_t y, natural_t msgType, void *msgArgument)
{
	int	result;
	char	*s;

/*
	fprintf (stderr, "powerCallback: message_type %08lx, arg %08lx\n",
		(long unsigned int) msgType, (long  unsigned int) msgArgument);
*/
	switch (msgType) {
	case kIOMessageCanSystemSleep :
		if (args.allowsleepcommand == NULL) {
			result = 0;
			message (LOG_INFO, "allow sleep\n");
		} else if (args.allowsleepcommand == DENY_SLEEP) {
			result = -1;
			message (LOG_INFO, "deny sleep\n");
		} else {
			result = system(args.allowsleepcommand);
			s = (! WIFEXITED(result) || WEXITSTATUS(result)) ? "deny" : "allow";
			message (LOG_INFO, "%s sleep: %s: exit=%s status=%d\n", s, args.allowsleepcommand,
				WIFEXITED(result) ? "ok" : "err", WEXITSTATUS(result));
			result = ! WIFEXITED(result) || WEXITSTATUS(result);
		}
		if (result)
			IOCancelPowerChange (* (io_connect_t *) rootPort, (long) msgArgument);
		else
			IOAllowPowerChange (* (io_connect_t *) rootPort, (long) msgArgument);
		break;
	case kIOMessageSystemWillSleep :
		if (args.sleepcommand)
			message (LOG_INFO, "sleep: %s: %d\n", args.sleepcommand, system(args.sleepcommand));
		IOAllowPowerChange (* (io_connect_t *) rootPort, (long) msgArgument);
		break;
	case kIOMessageSystemWillNotSleep :
		if (args.cantsleepcommand)
			message (LOG_INFO, "can't sleep: %s: %d\n", args.cantsleepcommand, system(args.cantsleepcommand));
		else
			message (LOG_INFO, "can't sleep\n");
		break;
	case kIOMessageSystemHasPoweredOn :
		setupIdleTimer ();
		if (args.wakeupcommand)
			message (LOG_INFO, "wakeup: %s: %d\n", args.wakeupcommand, system(args.wakeupcommand));
		break;
	}
}


static void initializePowerNotifications (void)
{
	static io_connect_t	rootPort;	/* used by powerCallback() via context pointer */
	
	IONotificationPortRef	notificationPort;
	io_object_t		notifier;

	rootPort = IORegisterForSystemPower(&rootPort, &notificationPort, powerCallback, &notifier);
	if (! rootPort) {
		message (LOG_ERR, "IORegisterForSystemPower failed\n");
		exit (1);
	}
	CFRunLoopAddSource (CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(notificationPort), kCFRunLoopDefaultMode);
}


void displayCallback (void *context, io_service_t y, natural_t msgType, void *msgArgument)
{
	static enum { displayOn, displayDimmed, displayOff } state = displayOn;
	
/*
	fprintf (stderr, "displayCallback: message_type %08lx, arg %08lx\n",
		(long unsigned int) msgType, (long  unsigned int) msgArgument);
*/
	switch (msgType) {
	case kIOMessageDeviceWillPowerOff :
		state++;
		if (state == displayDimmed && args.displaydimcommand)
			message (LOG_INFO, "displaydim: %s: %d\n", args.displaydimcommand,
				system(args.displaydimcommand));
		else if (state == displayOff && args.displaysleepcommand)
			message (LOG_INFO, "displaysleep: %s: %d\n", args.displaysleepcommand,
				system(args.displaysleepcommand));
		break;
	case kIOMessageDeviceHasPoweredOn :
		if (state == displayDimmed && args.displayundimcommand)
			message (LOG_INFO, "displayundim: %s: %d\n", args.displayundimcommand,
				system(args.displayundimcommand));
		else if (args.displaywakeupcommand)
			message (LOG_INFO, "displaywakeup: %s: %d\n", args.displaywakeupcommand,
				system(args.displaywakeupcommand));
		state = displayOn;
		break;
	}
}


static void initializeDisplayNotifications (void)
{
	io_service_t		displayWrangler;
	IONotificationPortRef	notificationPort;
	io_object_t		notifier;

	displayWrangler = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceNameMatching("IODisplayWrangler"));
	if (! displayWrangler) {
		message (LOG_ERR, "IOServiceGetMatchingService failed\n");
		exit (1);
	}
	notificationPort = IONotificationPortCreate(kIOMasterPortDefault);
	if (! notificationPort) {
		message (LOG_ERR, "IONotificationPortCreate failed\n");
		exit (1);
	}
	if (IOServiceAddInterestNotification(notificationPort, displayWrangler, kIOGeneralInterest,
		displayCallback, NULL, &notifier) != kIOReturnSuccess) {
		message (LOG_ERR, "IOServiceAddInterestNotification failed\n");
		exit (1);
	}
	CFRunLoopAddSource (CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(notificationPort), kCFRunLoopDefaultMode);
	IOObjectRelease (displayWrangler);
}


#define POWER_SOURCE_ERROR	-1	// error, don't assume power source changed
#define POWER_SOURCE_BATTERY	0	// not plugged in, using battery power
#define POWER_SOURCE_AC		1	// plugged in, using AC power


int getPowerSource (void)		// returns one of the three #defines above
{
	int result = POWER_SOURCE_ERROR;
	
	CFTypeRef info = NULL;
	CFArrayRef powerSources = NULL;
	CFTypeRef source = NULL;
	CFDictionaryRef description = NULL;
	CFStringRef powerSourceState = NULL;
	
	info = IOPSCopyPowerSourcesInfo();
	if (! info) goto ret;
	powerSources = IOPSCopyPowerSourcesList(info);
	if (! powerSources) goto ret;
	if (CFArrayGetCount(powerSources) == 0) goto ret;
	source = CFArrayGetValueAtIndex(powerSources, 0);
	if (! source) goto ret;
	description = IOPSGetPowerSourceDescription(info, source);
	if (! description) goto ret;
	powerSourceState = CFDictionaryGetValue(description, CFSTR(kIOPSPowerSourceStateKey));
	if (! powerSourceState) goto ret;
	result = (CFStringCompare(powerSourceState, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo) ?
		POWER_SOURCE_AC : POWER_SOURCE_BATTERY;
ret :
	if (info) CFRelease (info);
	if (powerSources) CFRelease (powerSources);
	//if (source) CFRelease (source);
	//if (description) CFRelease (description);
	//if (powerSourceState) CFRelease (powerSourceState);
	return result;
}


void powerSourceCallback (void *context)
{
	static int oldPowerSource = POWER_SOURCE_ERROR;
	
	int powerSource = getPowerSource();
	if (powerSource != POWER_SOURCE_ERROR && powerSource != oldPowerSource) {
		if (powerSource == POWER_SOURCE_AC) {
			if (args.plugcommand)
				message (LOG_INFO, "power plugged in: %s: %d\n", args.plugcommand,
					system(args.plugcommand));
		} else {
			if (args.unplugcommand)
				message (LOG_INFO, "power unplugged: %s: %d\n", args.unplugcommand,
					system(args.unplugcommand));
		}
		oldPowerSource = powerSource;
	}
	
}


static void initializePowerSourceNotifications (void)
{
	CFRunLoopSourceRef source = IOPSNotificationCreateRunLoopSource(powerSourceCallback, NULL);
	if (! source) {
		message(LOG_ERR, "IOPSNotificationCreateRunLoopSource failed\n");
		exit (1);
	}
	CFRunLoopAddSource (CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
}


static void signalCallback (int sig)
{
	switch (sig) {
	case SIGHUP :
		message (LOG_INFO, "got SIGHUP - reconfiguring\n");	
		parseArgs (args.argc, args.argv);
		setupIdleTimer ();
		break;
	case SIGTERM :
	case SIGINT :
		message (LOG_INFO, "got %s - exiting\n", sig == SIGTERM ? "SIGTERM" : "SIGINT");	
		writePidFile (NULL);
		exit (0);
		break;
	}
}


int main (int argc, char * const *argv)
{
	parseArgs (argc, argv);
	if (args.daemon) {
		if (daemon(0, 0)) {
			message (LOG_ERR, "daemonizing failed: %d\n", errno);
			return (1);
		}
		writePidFile (args.pidfile);
		fclose (stdin);
		fclose (stdout);
		fclose (stderr);
	}
	signal (SIGHUP, signalCallback);
	signal (SIGINT, signalCallback);
	signal (SIGTERM, signalCallback);
	setupIdleTimer ();
	initializeResumeNotifications ();
	initializePowerNotifications ();
	initializeDisplayNotifications ();
	initializePowerSourceNotifications ();
	CFRunLoopRun ();
	return (0);
}
