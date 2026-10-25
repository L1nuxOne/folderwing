"""Aggregate retained prototype receipts; never count unsupported formats as passes."""
from pathlib import Path
import csv, hashlib, json, statistics
ROOT=Path('/mnt/c/Work/openpilot')
COHORT=ROOT/'build/media_prototype/20261023'
def read(p): return json.loads(p.read_text(encoding='utf-8-sig'))
def stats(v):
    return dict(n=len(v), median=statistics.median(v), minimum=min(v), maximum=max(v)) if v else None
def success(row):
    r=row['result']
    return bool(row['process']['exit_code']==0 and not row['process']['timeout'] and r and r['ready'] and not r['error'] and not r['render_error'] and all(r[k] for k in ('volume_mute_ok','pause_ok','seek_ok','resume_ok')) and r['play_position_advance_s']>.1)
metrics=['first_verified_render_from_launch_ms','ready_from_launch_ms','ready_private_bytes','ready_working_set','seek_ms','engine_close_ms','cpu_play_ms','gpu_video_engine_percent','gpu_3d_percent']
all_rows=[]; summaries={}
for name in ('video-comparison-1','comparison-2'):
    receipt=read(COHORT/name/'RESULT.json')
    assert receipt['completed'] and not receipt['input_changed'], receipt
    assert receipt.get('final_endpoint_exit',0)==0
    rows=read(COHORT/name/'ROWS.json'); assert len(rows)==receipt['runs']
    groups=[]
    for file in dict.fromkeys(x['file'] for x in rows):
        for engine in ('mf','libmpv'):
            group=[x for x in rows if x['file']==file and x['engine']==engine]
            passed=[x for x in group if success(x)]
            expected_reject=(engine=='mf' and file in ('hevc_aac.mp4','av1_aac.mp4','vp9_opus.webm','vp8_vorbis.webm','vorbis.ogg','opus.opus')) or file=='corrupt.mp4'
            assert all(not x['process']['timeout'] for x in group), group
            assert (not passed and all(x['result']['error'] for x in group)) if expected_reject else len(passed)==len(group), (name,file,engine)
            if not expected_reject:
                for x in passed:
                    r=x['result']
                    assert 4.9 <= r['duration_s'] <= 5.1
                    if x['audio_only']: assert r['has_audio'] and not r['has_video']
                    else:
                        assert r['has_video'] and r['nonblack_samples']>8 and r['first_frame_hash']!='0000000000000000' and r['seek_frame_hash']!='0000000000000000'
                        assert r['native_width']==(3840 if file=='h264_4k60.mp4' else 1280)
                        if name=='comparison-2': assert r['has_audio'], (file,engine,'audio missing')
            groups.append(dict(file=file,engine=engine,runs=len(group),passes=len(passed),expected_rejection=expected_reject,
                errors=sorted(set(f"0x{x['result']['error']&0xffffffff:08x}" for x in group)),
                metrics={k:stats([x['result'][k] for x in passed if x['result'][k]>=0]) for k in metrics},
                decoders=sorted(set(x['result']['decoder'] for x in passed))))
    summaries[name]=dict(groups=groups,runs=len(rows),passes=sum(success(x) for x in rows),rejections=sum(not success(x) for x in rows),host_cpu_percent=stats([x['host_cpu_percent'] for x in rows]),available_ram_bytes=stats([x['available_ram_bytes'] for x in rows]))
    for x in rows: all_rows.append(dict(matrix=name,file=x['file'],engine=x['engine'],repeat=x['repeat'],pass_controls=success(x),exit_code=x['process']['exit_code'],error=f"0x{x['result']['error']&0xffffffff:08x}",host_cpu_percent=x['host_cpu_percent'],available_ram_bytes=x['available_ram_bytes'],**{k:x['result'][k] for k in metrics}))
(COHORT/'SUMMARY.json').write_text(json.dumps(summaries,indent=2)+'\n')
with (COHORT/'MEASUREMENTS.csv').open('w',newline='') as f:
    writer=csv.DictWriter(f,fieldnames=list(all_rows[0]));writer.writeheader();writer.writerows(all_rows)
lines=['# Measured media prototype results','', 'Three fresh processes per engine/valid fixture; median [min–max]. Process-fresh, OS caches retained. Hidden 640×360 D3D11 output; verified buffer readback, not display presentation. MiB is private committed process memory; excludes dedicated VRAM.','']
for name,s in summaries.items():
    lines += [f'## {name}', '',f"{s['runs']} runs: {s['passes']} playback/control passes, {s['rejections']} explicit format/invalid-input rejections, no outer timeout. Host CPU range {s['host_cpu_percent']['minimum']:.1f}–{s['host_cpu_percent']['maximum']:.1f}%; variable contention limits timing conclusions.",'','| Fixture | Engine | Passes | First video / audio-ready ms | Private MiB | Seek ms | Close ms |','|---|---|---:|---:|---:|---:|---:|']
    def fmt(m, divisor=1): return '—' if m is None else f"{m['median']/divisor:.1f} [{m['minimum']/divisor:.1f}–{m['maximum']/divisor:.1f}]"
    for g in s['groups']:
        m=g['metrics']; audio=g['file'] in ('pcm.wav','mp3.mp3','aac.m4a','flac.flac','vorbis.ogg','opus.opus')
        first=m['ready_from_launch_ms' if audio else 'first_verified_render_from_launch_ms']
        lines.append(f"| {g['file']} | {g['engine']} | {g['passes']}/{g['runs']} | {fmt(first)} | {fmt(m['ready_private_bytes'],1048576)} | {fmt(m['seek_ms'])} | {fmt(m['engine_close_ms'])} |")
    lines += ['']
(COHORT/'MEASUREMENTS.md').write_text('\n'.join(lines)+'\n')
print(json.dumps({k:{a:v[a] for a in ('runs','passes','rejections','host_cpu_percent')} for k,v in summaries.items()},indent=2))
