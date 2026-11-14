#import <Foundation/Foundation.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
extern char **environ;
static NSString *script;
static CFStringRef serviceName;
static pid_t child;
static volatile sig_atomic_t stopping;
static int stopSent;
static CFAbsoluteTime stopSince, lastWake;
static int previousState=-99;
static int forced;
/* A shutdown that never finishes. Adobe's server can stay in state 3 (stopping) for good: its Java
   side stalls, or it believes its database is not running ("Cannot stop non-running database")
   and never asks it to quit, and then waits for it. The pane shows "Stopping Version Cue CS2..."
   with every control disabled, and a reinstall could not replace the controller. After
   VC_STOP_TIMEOUT seconds (default 90; a normal shutdown takes 10-20 s) the server's process
   group - the server and its database, which the controller started as a group of their own - is
   ended. Nothing writes to the database by then; InnoDB recovers its log at the next start. */
static double stopTimeout(void) { const char *e=getenv("VC_STOP_TIMEOUT"); double v=e?atof(e):90; return v>=10?v:90; }
static void forceStop(int serverPID) {
    pid_t group=getpgid(serverPID);
    forced=1;
    if(group==serverPID && group!=getpgrp()) {
        killpg(group,SIGKILL);
        fprintf(stderr,"vc-controller: server %d still stopping after %.0f s: ended it and its database (process group %d)\n",serverPID,stopTimeout(),(int)group);
    } else {
        kill(serverPID,SIGKILL);
        fprintf(stderr,"vc-controller: server %d still stopping after %.0f s: ended it\n",serverPID,stopTimeout());
    }
}
static void signalStop(int sig) { stopping=1; }
static void startServer(void) {
    if (child && (kill(child,0)==0)) return;
    CFStringRef status=CFStringCreateWithFormat(NULL,NULL,CFSTR("%@.status"),serviceName);
    CFMessagePortRef running=CFMessagePortCreateRemote(NULL,status); CFRelease(status);
    if(running) { CFRelease(running); return; }
    char *args[]={(char *)"/bin/bash",(char *)[script fileSystemRepresentation],(char *)"run",NULL};
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes,0);
    int result=posix_spawn(&child,"/bin/bash",NULL,&attributes,args,environ);
    posix_spawnattr_destroy(&attributes);
    fprintf(stderr,"vc-controller: start result=%d pid=%d\n",result,child);
    if(result) child=0;
}
static CFDataRef request(CFMessagePortRef port,SInt32 message,CFDataRef data,void *info) {
    fprintf(stderr,"vc-controller: request %d\n",(int)message);
    if(message==1 && !stopping) startServer();
    return NULL;
}
static CFMessagePortRef statusPort(void) {
    CFStringRef name=CFStringCreateWithFormat(NULL,NULL,CFSTR("%@.status"),serviceName);
    CFMessagePortRef port=CFMessagePortCreateRemote(NULL,name); CFRelease(name); return port;
}
static void tick(CFRunLoopTimerRef timer,void *info) {
    int status; pid_t exited;
    while((exited=waitpid(-1,&status,WNOHANG))>0) if(exited==child) { fprintf(stderr,"vc-controller: server exited status=%d\n",status); child=0; }
    int state=-1, serverPID=0;
    CFMessagePortRef port=statusPort();
    if(port) {
        CFDataRef reply=NULL;
        if(!CFMessagePortSendRequest(port,2,NULL,0.5,0.5,kCFRunLoopDefaultMode,&reply) && reply) {
            CFPropertyListRef value=CFPropertyListCreateFromXMLData(NULL,reply,kCFPropertyListImmutable,NULL);
            if(value && CFGetTypeID(value)==CFDictionaryGetTypeID()) {
                CFNumberRef n=CFDictionaryGetValue(value,CFSTR("VCServerState"));
                if(n && CFGetTypeID(n)==CFNumberGetTypeID()) CFNumberGetValue(n,kCFNumberIntType,&state);
                n=CFDictionaryGetValue(value,CFSTR("VCServerPID"));
                if(n && CFGetTypeID(n)==CFNumberGetTypeID()) CFNumberGetValue(n,kCFNumberIntType,&serverPID);
            }
            if(value)CFRelease(value); CFRelease(reply);
        }
        if(stopping && !stopSent) { CFMessagePortSendRequest(port,3,NULL,1,0,NULL,NULL); stopSent=1; }
        CFRelease(port);
    }
    if(state!=previousState) {fprintf(stderr,"vc-controller: state=%d server=%d\n",state,serverPID);previousState=state;}
    CFAbsoluteTime now=CFAbsoluteTimeGetCurrent();
    if(state==3) {
        if(!stopSince) stopSince=now;
        if(serverPID>1 && now-stopSince>=5 && now-lastWake>=2) {
            char pidbuf[32]; snprintf(pidbuf,sizeof pidbuf,"%d",serverPID);
            char *args[]={(char *)"/bin/bash",(char *)[script fileSystemRepresentation],(char *)"wake-db",pidbuf,NULL};
            pid_t helper=0; int result=posix_spawn(&helper,"/bin/bash",NULL,NULL,args,environ);
            fprintf(stderr,"vc-controller: database wake result=%d\n",result);lastWake=now;
        }
        if(serverPID>1 && !forced && now-stopSince>=stopTimeout()) forceStop(serverPID);
    } else {stopSince=0;lastWake=0;forced=0;}
    if(stopping && !child && state<0) CFRunLoopStop(CFRunLoopGetCurrent());
}
int main(int argc,const char **argv) {
    NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];
    if(argc!=3) {fprintf(stderr,"usage: controller DOMAIN SCRIPT\n");return 2;}
    serviceName=(CFStringRef)[[NSString alloc] initWithUTF8String:argv[1]];
    script=[[NSString alloc] initWithUTF8String:argv[2]];
    CFStringRef control=CFStringCreateWithFormat(NULL,NULL,CFSTR("%@.ctrl"),serviceName);
    CFMessagePortContext context={0,NULL,NULL,NULL,NULL};
    CFMessagePortRef port=CFMessagePortCreateLocal(NULL,control,request,&context,NULL);
    if(!port) {fprintf(stderr,"vc-controller: cannot register control port\n");return 1;}
    CFRunLoopSourceRef source=CFMessagePortCreateRunLoopSource(NULL,port,0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),source,kCFRunLoopDefaultMode);
    CFRunLoopTimerRef timer=CFRunLoopTimerCreate(NULL,CFAbsoluteTimeGetCurrent()+1,1,0,0,tick,NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(),timer,kCFRunLoopDefaultMode);
    signal(SIGTERM,signalStop); signal(SIGINT,signalStop);
    const char *pidfile=getenv("VC_CONTROLLER_PIDFILE");
    if(pidfile) {FILE *f=fopen(pidfile,"w");if(f){fprintf(f,"%d\n",getpid());fclose(f);}}
    fprintf(stderr,"vc-controller: ready\n");
    CFPropertyListRef automatic=CFPreferencesCopyAppValue(CFSTR("AutoStart"),serviceName);
    if(automatic && CFGetTypeID(automatic)==CFBooleanGetTypeID() && CFBooleanGetValue(automatic)) startServer();
    if(automatic)CFRelease(automatic);
    CFRunLoopRun();
    CFRunLoopTimerInvalidate(timer); CFRelease(timer);
    CFMessagePortInvalidate(port); CFRelease(source); CFRelease(port); CFRelease(control);
    if(pidfile)unlink(pidfile);
    [pool drain];return 0;
}
