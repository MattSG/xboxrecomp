"""Run from an MSVC developer prompt: python tests/test_menu_placement.py."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/video/nv2a_d3d11.c").read_text()
start = source.index("            if (tall && wide_x && s_draw_textured")
end = source.index("\n        }\n    }\n\n    /* Vertex data", start)
placement = source[start:end]
harness = r"""
#include <assert.h>
#include <stdint.h>
static int s_frame_bg_tex, s_draw_textured, s_draw_reads_wide, s_draw_tex_addr=1, race;
static int nv2a_d3d_hud_active(void) { return race && !s_frame_bg_tex; }
static int world_tag_draw(float a,float b,float c,float d,int t) { return 0; }
static int hud_split(void *p,int c,void *i,int l,int n,int t,float w,float r,int f,void *d) { return 0; }
static void check(int in_race,int background,int wide,int textured,int reads,float left,float right,float expected,float offset) {
    float narrow=.75f, squeeze=1, shift=0, yscale=1, xlo=left,xhi=right,ylo=0,yhi=480;
    int tall=background,wide_x=wide,count=0,dx_cap=0,use_dx=0,lo=0,ni=0,topology=0,gw=640;
    void *pos=0,*idx=0,*dx=0;
    race=in_race; s_frame_bg_tex=background==2; s_draw_textured=textured; s_draw_reads_wide=reads;
""" + placement + r"""
    assert(squeeze==expected && shift==offset && yscale==1);
}
int main(void) {
    check(0,0,0,1,0,100,200,1,0); /* menu controls */
    check(0,1,1,1,0,0,640,1,0); /* background */
    check(0,2,0,1,0,0,100,1,0); /* edge collage */
    check(1,1,1,1,0,0,640,1,0); /* menu entered from race */
    check(1,2,0,1,0,100,200,1,0); /* its controls */
    check(1,0,0,1,0,0,100,.75f,-.25f); /* left HUD */
    check(1,0,0,1,0,500,640,.75f,.25f); /* right HUD */
    check(1,0,1,1,1,0,640,1,0); /* wide render-target copy */
}
"""
with tempfile.TemporaryDirectory() as folder:
    test = Path(folder) / "menu.c"
    test.write_text(harness)
    subprocess.run(["cl", "/nologo", str(test), "/Fe:" + str(Path(folder) / "menu.exe"), "/Fo:" + str(Path(folder) / "menu.obj")], check=True)
    subprocess.run([str(Path(folder) / "menu.exe")], check=True)
print("Menu and race HUD placement checks passed")
