#!/usr/bin/python
"""Install/remove the system Version Cue CS2 controller without changing Adobe binaries."""
from __future__ import print_function
import argparse, errno, os, plistlib, pwd, shutil, subprocess, sys, tempfile, time

LABEL = 'io.github.rosettus.versioncue'
DOMAIN = 'com.adobe.versioncueCS2'
ORIGIN = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))

def call(args, quiet=False):
    if quiet:
        with open(os.devnull, 'wb') as sink:
            return subprocess.call(args, stdout=sink, stderr=sink)
    subprocess.check_call(args)

def mkdir(path, user=None):
    missing=[]
    p=path
    while not os.path.exists(p):
        missing.append(p); p=os.path.dirname(p)
    for p in reversed(missing):
        os.mkdir(p, 0o755)
        if user and os.geteuid()==0: os.chown(p,user.pw_uid,user.pw_gid)

def copy_file(source, target, mode=0o755):
    if os.path.realpath(source)==os.path.realpath(target): return
    mkdir(os.path.dirname(target))
    temporary=target+'.new'
    shutil.copyfile(source,temporary); os.chmod(temporary,mode); os.rename(temporary,target)

def read_plist(path):
    # plistlib in Mojave's Python 2 cannot read the binary plists CFPreferences writes.
    data=subprocess.check_output(['/usr/bin/plutil','-convert','xml1','-o','-',path])
    return plistlib.readPlistFromString(data)

def settings(user):
    result={}
    for p in ['/Library/Preferences/'+DOMAIN+'.plist',os.path.join(user.pw_dir,'Library/Preferences',DOMAIN+'.plist')]:
        if os.path.isfile(p): result.update(read_plist(p))
    return result

def agent_path(user):
    return os.path.join(user.pw_dir,'Library/LaunchAgents',LABEL+'.plist')

def remove_user(root,user):
    path=agent_path(user)
    if not os.path.isfile(path): return
    info=read_plist(path)
    if os.path.join(root,'versioncue/versioncue') not in info.get('ProgramArguments',[]):
        raise RuntimeError('Refusing to remove an agent belonging to another runtime: '+path)
    call(['/bin/launchctl','bootout','gui/%d/%s'%(user.pw_uid,LABEL)],quiet=True)
    pidfile=os.path.join(user.pw_dir,'Library/Logs/Rosettus/versioncue-controller.pid')
    try:
        pid=int(open(pidfile).read().strip())
    except (IOError,ValueError): pid=0
    deadline=time.time()+150
    while pid>1:
        try: os.kill(pid,0)
        except OSError as error:
            if error.errno==errno.ESRCH: break
            raise
        if time.time()>deadline:
            # The controller ends a stuck server shutdown itself after 90 s (VC_STOP_TIMEOUT); one
            # still running now is stuck as well. End it, so the service is installed and started
            # again: failing here left it unloaded, and Adobe's pane showed "not available".
            sys.stderr.write('install_versioncue: controller %d did not stop in time; ended it\n'%pid)
            try: os.kill(pid,9)
            except OSError: pass
            break
        time.sleep(1)
    os.unlink(path)
    print('Removed Version Cue agent for '+user.pw_name)

def remove_system(root):
    path='/Library/LaunchDaemons/'+LABEL+'.plist'
    if not os.path.isfile(path): return
    info=read_plist(path)
    if os.path.join(root,'versioncue/versioncue') not in info.get('ProgramArguments',[]):
        return  # Another Rosettus installation owns this system service.
    if os.geteuid()!=0: raise RuntimeError('Removing the system service requires root.')
    call(['/bin/launchctl','bootout','system/'+LABEL],quiet=True)
    try: pid=int(open('/Library/Logs/Rosettus/versioncue-controller.pid').read().strip())
    except (IOError,ValueError): pid=0
    deadline=time.time()+150
    while pid>1:
        try: os.kill(pid,0)
        except OSError as error:
            if error.errno==errno.ESRCH: break
            raise
        if time.time()>deadline:
            # The controller ends a stuck server shutdown itself after 90 s (VC_STOP_TIMEOUT); one
            # still running now is stuck as well. End it, so the service is installed and started
            # again: failing here left it unloaded, and Adobe's pane showed "not available".
            sys.stderr.write('install_versioncue: controller %d did not stop in time; ended it\n'%pid)
            try: os.kill(pid,9)
            except OSError: pass
            break
        time.sleep(1)
    os.unlink(path)
    print('Removed Version Cue system service')

def install(root,user):
    if os.geteuid()!=0: raise RuntimeError('The Version Cue system service requires root (sudo).')
    config=settings(user)
    app=os.path.realpath(config.get('VersionCuePath','/Applications/Adobe Version Cue CS2'))
    executable=os.path.join(app,'bin/VersionCueCS2.app/Contents/MacOS/VersionCueCS2')
    if not os.path.isfile(executable): raise RuntimeError('Adobe Version Cue CS2 is not installed at '+app)
    jvm=os.path.join(root,'System/Library/Frameworks/JavaVM.framework/Versions/1.5.0/Libraries/libclient.dylib')
    if not os.path.isfile(jvm): raise RuntimeError('Install the optional PowerPC Java 5 export first.')
    daemon='/Library/LaunchDaemons/'+LABEL+'.plist'
    if os.path.isfile(daemon) and os.path.join(root,'versioncue/versioncue') not in read_plist(daemon).get('ProgramArguments',[]):
        raise RuntimeError('A different Rosettus runtime owns the existing system service.')
    remove_system(root)
    remove_user(root,user)
    destination=os.path.join(root,'versioncue')
    mkdir(destination)
    built=os.path.join(ORIGIN,'bin/versioncue')
    if os.path.isdir(built):
        for name in ['controller','libvcjvm.dylib','nullchecks.jar','ASM-LICENSE.txt']:
            copy_file(os.path.join(built,name),os.path.join(destination,name),0o755 if name in ('controller','libvcjvm.dylib') else 0o644)
    for name in ['controller','libvcjvm.dylib','nullchecks.jar']:
        if not os.path.isfile(os.path.join(destination,name)): raise RuntimeError('Missing Rosettus support file: '+name)
    wrapper=os.path.join(ORIGIN,'scripts/versioncue')
    if os.path.isfile(wrapper): copy_file(wrapper,os.path.join(destination,'versioncue'))
    copy_file(os.path.join(ORIGIN,'tools/versioncue_wake_db.py'),os.path.join(root,'tools/versioncue_wake_db.py'))
    source=os.path.join(app,'bin/VersionCueCS2.app')
    temporary=tempfile.mkdtemp(prefix='vcapp.',dir=destination)
    staging=os.path.join(temporary,'VersionCueCS2.app')
    try:
        shutil.copytree(source,staging,symlinks=True)
        program=os.path.join(staging,'Contents/MacOS/VersionCueCS2')
        os.chmod(program,0o755)
        call(['/usr/bin/install_name_tool','-change','/System/Library/Frameworks/JavaVM.framework/Versions/A/JavaVM',os.path.join(destination,'libvcjvm.dylib'),program])
        final=os.path.join(destination,'VersionCueCS2.app')
        if os.path.exists(final): shutil.rmtree(final)
        os.rename(staging,final)
    finally: shutil.rmtree(temporary)
    # The uninitialized CS2 workspace expects this directory when its scheduler flushes.
    data=config.get('DataPath','data')
    if not os.path.isabs(data): data=os.path.join(app,data)
    mkdir(os.path.join(data,'content/0'),user)
    logs='/Library/Logs/Rosettus'; mkdir(logs)
    path='/Library/LaunchDaemons/'+LABEL+'.plist'; mkdir(os.path.dirname(path))
    info={'Label':LABEL,'ProgramArguments':[os.path.join(destination,'versioncue'),'controller'],
          'RunAtLoad':True,'KeepAlive':True,'ThrottleInterval':5,'ExitTimeOut':180,
          'StandardOutPath':os.path.join(logs,'VersionCue-service.log'),
          'StandardErrorPath':os.path.join(logs,'VersionCue-service.log')}
    plistlib.writePlist(info,path); os.chmod(path,0o644)
    os.chown(path,0,0)
    call(['/bin/launchctl','bootstrap','system',path])
    print('Installed Version Cue background system service')
    print('Controller: '+os.path.join(destination,'versioncue'))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',default='/Library/Rosettus')
    parser.add_argument('--user')
    parser.add_argument('--remove',action='store_true')
    parser.add_argument('--all-users',action='store_true')
    args=parser.parse_args(); root=os.path.realpath(args.root)
    if args.all_users:
        if not args.remove: parser.error('--all-users is only supported with --remove')
        remove_system(root)
        for user in pwd.getpwall():
            if user.pw_uid>=500 and os.path.isfile(agent_path(user)): remove_user(root,user)
        return
    name=args.user or os.environ.get('SUDO_USER')
    if not name or name=='root':
        name=subprocess.check_output(['/usr/bin/stat','-f','%Su','/dev/console']).strip()
    if name in ('loginwindow',''): name='root'
    user=pwd.getpwnam(name)
    if args.remove:
        remove_system(root)
        remove_user(root,user)
    else:
        try: install(root,user)
        except Exception:
            # never leave an installed service unloaded because a step failed
            daemon='/Library/LaunchDaemons/'+LABEL+'.plist'
            if os.path.isfile(daemon) and os.geteuid()==0: call(['/bin/launchctl','bootstrap','system',daemon],quiet=True)
            raise

if __name__=='__main__':
    try: main()
    except Exception as error:
        sys.stderr.write('install_versioncue: %s\n'%error); sys.exit(1)
