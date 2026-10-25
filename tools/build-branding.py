"""Export the native vector master to PNG and Windows ICO (CairoSVG + Pillow)."""
from pathlib import Path
import hashlib, io, json
import cairosvg
from PIL import Image
root=Path(__file__).resolve().parents[1]
assets=root/'docs/branding'
svg=assets/'folderwing-icon.svg'
png=cairosvg.svg2png(url=str(svg),output_width=1024,output_height=1024)
(assets/'folderwing-icon.png').write_bytes(png)
image=Image.open(io.BytesIO(png)).convert('RGBA')
sizes=[16,20,24,32,40,48,64,96,128,256]
image.save(assets/'folderwing.ico',format='ICO',sizes=[(n,n) for n in sizes])
manifest={'schema':'folderwing-brand-assets/v1','name':'Folderwing','palette':{'coral':'#FF6655','plum':'#30213C','cream':'#FFF7EF'},'icon_sizes':sizes,'icon_master':'folderwing-icon.svg','wordmark_origin':'Built-in image_gen, edited from the chosen Folderwing concept','files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [svg,assets/'folderwing-icon.png',assets/'folderwing.ico',assets/'folderwing-wordmark.png']}}
(assets/'assets.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Exported Folderwing PNG and ICO:',sizes)
