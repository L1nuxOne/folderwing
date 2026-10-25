"""Generate only synthetic media in a new owned directory using an explicit FFmpeg."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess

VIDEO_NAMES=('h264_aac.mp4','h264_aac.mkv','h264_4k60.mp4','hevc_aac.mp4','av1_aac.mp4','vp8_vorbis.webm','vp9_opus.webm','mpeg4_pcm.avi')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ffmpeg',type=Path,required=True);p.add_argument('--output-directory',type=Path,required=True);a=p.parse_args()
    if a.output_directory.exists():raise SystemExit('Use a new fixture output directory; never overwrite fixtures')
    ffmpeg=a.ffmpeg.resolve();out=a.output_directory.resolve();av=out/'fixtures';video_only=out/'video-only';av.mkdir(parents=True);video_only.mkdir()
    base=[str(ffmpeg),'-hide_banner','-nostdin','-loglevel','error','-threads','2','-filter_threads','2']
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
      'pcm.wav':audio+['-c:a','pcm_s16le'],'mp3.mp3':audio+['-c:a','libmp3lame','-b:a','160k'],
      'aac.m4a':audio+['-c:a','aac','-b:a','160k'],'flac.flac':audio+['-c:a','flac'],
      'vorbis.ogg':audio+['-c:a','libvorbis'],'opus.opus':audio+['-c:a','libopus']}
    commands=[]
    def run(args,target):
        command=base+args+['-n',str(target)];print('Generate',target.relative_to(out),flush=True)
        result=subprocess.run(command,capture_output=True,text=True,timeout=180)
        (out/(target.name+'.generation.log')).write_text(result.stderr)
        if result.returncode:raise RuntimeError('FFmpeg failed: '+result.stderr[-2000:])
        commands.append(command)
    for name,args in recipes.items():run(args,av/name)
    run(['-i',str(av/'h264_aac.mp4'),'-c','copy'],av/'h264_aac.mkv')
    for name in VIDEO_NAMES:run(['-i',str(av/name),'-map','0:v:0','-c','copy','-an'],video_only/name)
    (av/'corrupt.mp4').write_bytes(b'Folderwing deliberately invalid synthetic media\x00\xff')
    # Validate the tool produced the requested video/audio tracks, rather than
    # accepting encoder output solely from a successful process exit.
    for name in VIDEO_NAMES:
        inspected=subprocess.run([str(ffmpeg),'-hide_banner','-nostdin','-threads','2','-i',str(video_only/name),'-map','0:v:0','-frames:v','1','-f','null','-'],capture_output=True,text=True,timeout=30)
        if inspected.returncode or not re.search(r'Stream.*Video:',inspected.stderr) or re.search(r'Stream.*Audio:',inspected.stderr):raise RuntimeError('Expected a decodable video-only stream: '+name)
    sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
    manifest={'schema':'folderwing-synthetic-media/v1','ffmpeg_sha256':sha(ffmpeg),'ffmpeg_version':subprocess.check_output([str(ffmpeg),'-version'],text=True).splitlines()[0],'generator_sha256':sha(Path(__file__)),'commands':commands,'files':{x.relative_to(out).as_posix():sha(x) for d in [av,video_only] for x in sorted(d.iterdir())},'personal_media_used':False}
    (out/'FIXTURES.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
