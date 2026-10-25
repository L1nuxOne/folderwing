from pathlib import Path
import subprocess,hashlib,json,sys
root=Path('/mnt/c/Work/openpilot');cohort=root/'build/media_prototype/20261023';out=cohort/'fixtures';out.mkdir(exist_ok=True);exe=cohort/'dependencies/ffmpeg/ffmpeg.exe';w=lambda p:'C:/'+str(p).split('/mnt/c/',1)[1]
# Own synthetic chart and silent-by-default playback tone; no personal media.
base=[str(exe),'-hide_banner','-nostdin','-loglevel','error','-threads','2','-filter_threads','2']
video=['-f','lavfi','-i','testsrc2=size=1280x720:rate=30','-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','5']
audio=['-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','5']
recipes={
 'h264_4k60.mp4':['-f','lavfi','-i','testsrc2=size=3840x2160:rate=60','-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','5','-c:v','libx264','-threads','2','-preset','veryfast','-crf','24','-pix_fmt','yuv420p','-g','60','-c:a','aac','-movflags','+faststart'],
 'h264_aac.mp4':video+['-c:v','libx264','-threads','2','-preset','ultrafast','-crf','24','-pix_fmt','yuv420p','-g','30','-c:a','aac','-movflags','+faststart'],
 'hevc_aac.mp4':video+['-c:v','libx265','-threads','2','-preset','ultrafast','-x265-params','pools=2:frame-threads=2:log-level=error','-crf','28','-pix_fmt','yuv420p','-tag:v','hvc1','-c:a','aac','-movflags','+faststart'],
 'vp9_opus.webm':video+['-c:v','libvpx-vp9','-threads','2','-deadline','realtime','-cpu-used','8','-b:v','1200k','-c:a','libopus'],
 'vp8_vorbis.webm':video+['-c:v','libvpx','-threads','2','-deadline','realtime','-cpu-used','8','-b:v','1200k','-c:a','libvorbis'],
 'av1_aac.mp4':video+['-c:v','libsvtav1','-threads','2','-preset','12','-svtav1-params','lp=2','-crf','38','-pix_fmt','yuv420p','-c:a','aac','-movflags','+faststart'],
 'mpeg4_pcm.avi':video+['-c:v','mpeg4','-threads','2','-q:v','8','-c:a','pcm_s16le'],
 'pcm.wav':audio+['-c:a','pcm_s16le'], 'mp3.mp3':audio+['-c:a','libmp3lame','-b:a','160k'],
 'aac.m4a':audio+['-c:a','aac','-b:a','160k'], 'flac.flac':audio+['-c:a','flac'],
 'vorbis.ogg':audio+['-c:a','libvorbis'], 'opus.opus':audio+['-c:a','libopus']}
records=[]
for name,args in recipes.items():
 if len(sys.argv)>1 and name not in sys.argv[1:]:continue
 target=out/name
 if target.exists():print('Keep',name,flush=True);continue
 cmd=base+args+['-y',w(target)];print('Generate',name,flush=True);p=subprocess.run(cmd,capture_output=True,timeout=120);(cohort/(name+'-generation.log')).write_bytes(p.stderr);assert not p.returncode,(name,p.stderr[-2000:])
 records.append({'name':name,'bytes':target.stat().st_size,'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'command':cmd})
source=out/'h264_aac.mp4'
if source.exists() and (not len(sys.argv)>1 or 'h264_aac.mkv' in sys.argv[1:]):
 target=out/'h264_aac.mkv'
 if not target.exists():
  cmd=base+['-i',w(source),'-c','copy','-y',w(target)];p=subprocess.run(cmd,capture_output=True,timeout=30);assert not p.returncode,p.stderr;records.append({'name':target.name,'bytes':target.stat().st_size,'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'command':cmd})
old=json.loads((cohort/'FIXTURES.json').read_text()) if (cohort/'FIXTURES.json').exists() else [];names={r['name']:r for r in old};names.update({r['name']:r for r in records});(cohort/'FIXTURES.json').write_text(json.dumps(list(names.values()),indent=2))
