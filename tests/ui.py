#!/usr/bin/env python3
"""Disposable display/bus FLTK, URI dispatch, checkpoint and tray QA."""
import asyncio,hashlib,json,os,pathlib,re,signal,ssl,subprocess,sys,tempfile,time
from pathlib import Path
if '--watcher' in sys.argv:
 from gi.repository import Gio,GLib
 connection=Gio.bus_get_sync(Gio.BusType.SESSION,None)
 registered_services=[]
 xml='<node><interface name="org.kde.StatusNotifierWatcher"><method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method><property name="RegisteredStatusNotifierItems" type="as" access="read"/></interface></node>'
 info=Gio.DBusNodeInfo.new_for_xml(xml)
 def registered(connection,sender,path,interface,method,parameters,invocation):
  service=parameters.unpack()[0];registered_services.append(service+'/StatusNotifierItem');Path(sys.argv[-1]).write_text(service);invocation.return_value(None)
 def property_value(connection,sender,path,interface,name):return GLib.Variant('as',registered_services)
 connection.register_object('/StatusNotifierWatcher',info.interfaces[0],registered,property_value,None)
 Gio.bus_own_name_on_connection(connection,'org.kde.StatusNotifierWatcher',Gio.BusNameOwnerFlags.NONE,None,None)
 if os.environ.get('DOME_IDENTITIES_WATCHER_ONLY')!='1':Gio.bus_own_name_on_connection(connection,'com.canonical.AppMenu.Registrar',Gio.BusNameOwnerFlags.NONE,None,None)
 print('ready',flush=True);GLib.MainLoop().run();sys.exit()
from Xlib import X,display
from PIL import Image
signal.pthread_sigmask(signal.SIG_SETMASK,[])
if os.environ.get('DOME_TORRENT_TEST_BUS')!='1':
 os.execvpe('dbus-run-session',['dbus-run-session','--',sys.executable,*sys.argv],{**os.environ,'DOME_TORRENT_TEST_BUS':'1'})
root=Path(__file__).resolve().parents[1]
binary=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root/'build/dome-torrent'
out=root/'build/test-artifacts';out.mkdir(parents=True,exist_ok=True)
def wait(fn,seconds=15):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  result=fn()
  if result:return result
  time.sleep(.05)
 raise AssertionError('UI timed out; inspect '+str(out))
def magnet(number):return 'magnet:?xt=urn:btih:'+str(number)*40+'&dn=Fixture-'+str(number)
with tempfile.TemporaryDirectory(prefix='dome-torrent-ui-') as temporary:
 tmp=Path(temporary)
 read,write=os.pipe();xvfb=subprocess.Popen(['Xvfb','-displayfd',str(write),'-screen','0','1400x1000x24','-nolisten','tcp','-noreset'],pass_fds=(write,),stderr=subprocess.DEVNULL);os.close(write)
 with os.fdopen(read) as pipe:screen=':'+pipe.readline().strip()
 env={**os.environ,'HOME':str(tmp),'XDG_DATA_HOME':str(tmp/'data'),'XDG_CONFIG_HOME':str(tmp/'config'),'XDG_CACHE_HOME':str(tmp/'cache'),'XDG_STATE_HOME':str(tmp/'state'),'PATH':str(binary.parent)+':'+os.environ['PATH'],'XDG_CURRENT_DESKTOP':'Dome','DISPLAY':screen,'FLTK_BACKEND':'x11','LIBGL_ALWAYS_SOFTWARE':'1'}
 env.pop('WAYLAND_DISPLAY',None)
 env.pop('WAYLAND_SOCKET',None)
 apps=tmp/'data/applications';apps.mkdir(parents=True)
 desktop=root/'build/org.domed.Torrent.desktop'
 (apps/'org.domed.Torrent.desktop').write_text(desktop.read_text())
 subprocess.run(['update-desktop-database',str(apps)],env=env,check=True)
 config=tmp/'config';config.mkdir()
 (config/'dome-mimeapps.list').write_text('[Default Applications]\napplication/x-bittorrent=org.domed.Torrent.desktop;\nx-scheme-handler/magnet=org.domed.Torrent.desktop;\n')

 watcher=subprocess.Popen([sys.executable,__file__,'--watcher',str(tmp/'registered')],stdout=subprocess.PIPE,text=True);assert watcher.stdout.readline().strip()=='ready'
 log=(out/'client.log').open('w');app=subprocess.Popen([str(binary),'--local-only',magnet(1)],env=env,stdout=log,stderr=log);xd=display.Display(screen)
 try:
  def ready():
   if app.poll() is not None:raise AssertionError('Client exited '+str(app.returncode))
   return (tmp/'registered').exists()
  wait(ready);service=(tmp/'registered').read_text()
  def call(interface,method,*args,path='/StatusNotifierItem'):
   return subprocess.check_output(['gdbus','call','--session','--dest',service,'--object-path',path,'--method',interface+'.'+method,'--',*args],text=True,timeout=3)
  def prop(name):return call('org.freedesktop.DBus.Properties','Get','org.kde.StatusNotifierItem',name)
  assert 'dome-torrent' in prop('Id');assert '24, 24' in prop('IconPixmap')
  menu=re.search("'([^']+)'",prop('Menu')).group(1)
  def command(name):
   layout=call('com.canonical.dbusmenu','GetLayout','0','-1','[]',path=menu)
   item=re.search(r"\((\d+), \{'label': <'"+re.escape(name)+"'>",layout);assert item,name
   call('com.canonical.dbusmenu','Event',item[1],'clicked','<0>','0',path=menu)
  def window():return next((w for w in xd.screen().root.query_tree().children if w.get_wm_name()=='Dome Torrent'),None)
  w=wait(window);state=tmp/'state/de-xr/dome-torrent'
  wait(lambda:len(list(state.glob('*.resume')))==1)
  def input(*args):subprocess.run(['xdotool',*args],env=env,check=True,stdout=subprocess.DEVNULL)
  w.set_input_focus(X.RevertToParent,X.CurrentTime);xd.sync();time.sleep(.3)
  input('mousemove','--window',str(w.id),'70','200');input('click','1');time.sleep(.2)
  command('Pause');time.sleep(.3)
  # Real text-field input and Enter add a second magnet without a torrent file.
  command('Add magnet');input('type','--clearmodifiers','--delay','1',magnet(2));input('key','Return')
  wait(lambda:len(list(state.glob('*.resume')))==2)
  # A second launcher forwards URLs to the owning GApplication.
  forwarded=subprocess.run(['gio','open',magnet(3)],env=env,capture_output=True,text=True,timeout=8)
  assert forwarded.returncode==0,forwarded.stderr
  wait(lambda:len(list(state.glob('*.resume')))==3)
  torrent_file=tmp/'literal space $ ü.torrent'
  torrent_file.write_bytes(b'd4:infod6:lengthi4e4:name12:fixture.data12:piece lengthi16384e6:pieces20:'+b'x'*20+b'ee')
  subprocess.run(['gio','open',torrent_file.as_uri()],env=env,check=True,timeout=8)
  wait(lambda:len(list(state.glob('*.resume')))==4)
  assert app.poll() is None
  command('Add magnet');input('type','--clearmodifiers','--delay','1','magnet:?xt=bad');input('key','Return');time.sleep(.4)
  assert len(list(state.glob('*.resume')))==4 and app.poll() is None
  geometry=w.get_geometry();raw=w.get_image(0,0,geometry.width,geometry.height,X.ZPixmap,0xffffffff)
  Image.frombytes('RGB',(geometry.width,geometry.height),raw.data,'raw','BGRX').save(out/'client.png')
  command('Close to tray');wait(lambda:window() is None)
  call('org.kde.StatusNotifierItem','Activate','0','0');w=wait(window);wait(lambda:w.get_attributes().map_state==X.IsViewable)
  command('Quit');assert app.wait(timeout=15)==0
  assert len(list(state.glob('*.resume')))==4
  for file in state.glob('*.resume'):assert file.stat().st_mode&0o777==0o600
  (tmp/'registered').unlink()
  app=subprocess.Popen([str(binary),'--local-only','--tray'],env=env,stdout=log,stderr=log)
  wait(ready);service=(tmp/'registered').read_text();menu=re.search("'([^']+)'",prop('Menu')).group(1)
  time.sleep(.5);assert window() is None
  call('org.kde.StatusNotifierItem','Activate','0','0');wait(window)
  command('Quit');assert app.wait(timeout=15)==0
  print('PASS FLTK magnet text input, invalid link, Dome MIME file/magnet dispatch to an existing instance, universal commands, tray pixmap/restore, checkpoint permissions, restart and clean quit')
 finally:
  if app.poll() is None:app.terminate();app.wait(timeout=15)
  watcher.terminate();watcher.wait(timeout=5);xd.close();xvfb.terminate();xvfb.wait(timeout=5);log.close()
