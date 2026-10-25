#include "../src/namecodec.h"
/* termutil.c - see termutil.h */
#include "termutil.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnetwk.h>
#endif

static int ieq(const wchar_t *a, const wchar_t *b)
{
    for (; *a && *b; a++, b++) {
        wchar_t x = *a >= L'A' && *a <= L'Z' ? *a + 32 : *a, y = *b >= L'A' && *b <= L'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

int term_shell_kind(const wchar_t *exe)
{
    if (!exe) return TERM_SH_OTHER;
    const wchar_t *name = exe;
    for (const wchar_t *p = exe; *p; p++)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    wchar_t base[64];
    size_t n = 0;
    while (name[n] && name[n] != L'"' && name[n] != L' ' && n < 63) base[n] = name[n], n++;
    base[n] = 0;
    if (ieq(base, L"cmd.exe") || ieq(base, L"cmd")) return TERM_SH_CMD;
    if (ieq(base, L"pwsh.exe") || ieq(base, L"pwsh") || ieq(base, L"powershell.exe") || ieq(base, L"powershell"))
        return TERM_SH_PWSH;
    if (ieq(base,L"wsl.exe") || ieq(base,L"wsl")) return TERM_SH_WSL;
    return TERM_SH_OTHER;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }


static int wsl_split_kind(const wchar_t *path,wchar_t *distro,size_t dc,wchar_t *linux,size_t lc)
{
    size_t start=0;
#ifdef _WIN32
    wchar_t mapped_path[4096];
    if(path[0]&&path[1]==L':'){
        wchar_t drive[4]={path[0],L':',L'\\',0},local[3]={path[0],L':',0},share[260];DWORD n=260;
        if(GetDriveTypeW(drive)==DRIVE_REMOTE&&WNetGetConnectionW(local,share,&n)==NO_ERROR){
            if(wcslen(share)+wcslen(path+2)+1>=4096)return 0;
            wcscpy(mapped_path,share);wcscat(mapped_path,path+2);path=mapped_path;
        }
    }
#endif
    if(!_wcsnicmp(path,L"\\\\wsl.localhost\\",16))start=16;
    else if(!_wcsnicmp(path,L"\\\\wsl$\\",7))start=7;
    else return 0;
    const wchar_t *p=path+start,*end=wcschr(p,L'\\');size_t n=end?(size_t)(end-p):wcslen(p);
    if(!n||n>=dc||lc<2)return -1;
    for(size_t i=0;i<n;i++)if(!((p[i]>=L'a'&&p[i]<=L'z')||(p[i]>=L'A'&&p[i]<=L'Z')||(p[i]>=L'0'&&p[i]<=L'9')||wcschr(L"._-",p[i])))return -1;
    memcpy(distro,p,n*sizeof(wchar_t));distro[n]=0;
    linux[0]=L'/';p=end?end+1:p+n;
    return fpe_lx_literal(p,linux+1,lc-1,1)?1:-1;
}
int term_wsl_split(const wchar_t *path,wchar_t *distro,size_t dc,wchar_t *linux,size_t lc)
{return wsl_split_kind(path,distro,dc,linux,lc)>0;}
static int path_bytes(const wchar_t *path,int native,unsigned char *out,size_t cap,size_t *len)
{
    size_t k=0;
    for(size_t i=0;path[i];i++){
        uint32_t c=(uint16_t)path[i];unsigned char b[4];size_t n;
        if(c>=0xD800&&c<=0xDBFF){uint32_t d=(uint16_t)path[++i];if(d<0xDC00||d>0xDFFF)return 0;c=0x10000+((c-0xD800)<<10)+d-0xDC00;}
        else if(c>=0xDC80&&c<=0xDCFF&&native){b[0]=(unsigned char)(c-0xDC00);n=1;goto put;}
        else if(c>=0xDC00&&c<=0xDFFF)return 0;
        if(c<0x80){b[0]=(unsigned char)c;n=1;}
        else if(c<0x800){b[0]=0xC0|(c>>6);b[1]=0x80|(c&63);n=2;}
        else if(c<0x10000){b[0]=0xE0|(c>>12);b[1]=0x80|((c>>6)&63);b[2]=0x80|(c&63);n=3;}
        else{b[0]=0xF0|(c>>18);b[1]=0x80|((c>>12)&63);b[2]=0x80|((c>>6)&63);b[3]=0x80|(c&63);n=4;}
put:   if(k+n>cap)return 0;memcpy(out+k,b,n);k+=n;
    }
    *len=k;return 1;
}
int term_wsl_arg(const wchar_t *path,wchar_t *out,size_t cap)
{
    wchar_t distro[128],linux[2048];int native=wsl_split_kind(path,distro,128,linux,2048);if(native<0)return 0;
    unsigned char bytes[8192];size_t n;static const wchar_t hex[]=L"0123456789abcdef";
    if(!path_bytes(native?linux:path,native,bytes,sizeof bytes,&n)||2*n+2>cap)return 0;
    out[0]=native?L'x':L'w';
    for(size_t i=0;i<n;i++){out[1+2*i]=hex[bytes[i]>>4];out[2+2*i]=hex[bytes[i]&15];}
    out[1+2*n]=0;return 1;
}
static int wsl_osc(const char *s,size_t n,wchar_t *out,size_t cap)
{
    const char *end=memchr(s,';',n);if(!end)return 0;
    size_t dn=(size_t)(end-s),hn=n-dn-1;
    if(!dn||dn>=128||!hn||(hn&1)||hn>=8192)return 0;
    wchar_t distro[128];
    for(size_t i=0;i<dn;i++){char c=s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;distro[i]=(wchar_t)c;}distro[dn]=0;
    unsigned char raw[4096];size_t rn=hn/2;
    for(size_t i=0;i<rn;i++){int a=hexv(end[1+2*i]),b=hexv(end[2+2*i]);if(a<0||b<0||!(a*16+b))return 0;raw[i]=(unsigned char)(a*16+b);}
    if(raw[0]!='/')return 0;
    int k=swprintf(out,cap,L"\\\\wsl.localhost\\%ls",distro);if(k<0)return 0;
    size_t units=0;
    return fpe_lx_from_bytes(raw,rn,out+k,cap-(size_t)k,1,1,&units);
    out[k]=0;return 1;
}
static int bash_cd(const wchar_t *from,const wchar_t *dir,wchar_t *out,size_t cap)
{
    wchar_t distro[128],linux[2048],fdistro[128],flinux[2048];
    int native=wsl_split_kind(dir,distro,128,linux,2048);if(native<0)return 0;
    if(native&&from){int prior=wsl_split_kind(from,fdistro,128,flinux,2048);if(prior<0||(prior&&!ieq(distro,fdistro)))return 0;}
    unsigned char b[8192];size_t n;
    if(!path_bytes(native?linux:dir,native,b,sizeof b,&n))return 0;
    const wchar_t *head=native?L"builtin cd -- $'":L"builtin cd -- \"$(wslpath -u $'";
    size_t k=wcslen(head);if(k+4*n+5>=cap)return 0;wcscpy(out,head);
    static const wchar_t hex[]=L"0123456789abcdef";
    for(size_t i=0;i<n;i++){out[k++]=L'\\';out[k++]=L'x';out[k++]=hex[b[i]>>4];out[k++]=hex[b[i]&15];}
    wcscpy(out+k,native?L"'":L"')\"");return 1;
}
/* UTF-8 bytes to a path: / becomes \, a leading /C: loses its slash */
static int to_path(const char *s, size_t n, int unescape, wchar_t *out, size_t cap)
{
    char buf[2048];
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < sizeof buf; i++) {
        if (unescape && s[i] == '%' && i + 2 < n + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            buf[k++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        } else {
            buf[k++] = s[i];
        }
    }
    buf[k] = 0;
    char *p = buf;
    if (p[0] == '/' && p[1] && p[2] == ':') p++;
    if (!p[0] || cap < 4) return 0;
    wchar_t tmp[1024];
    int w = 0;
    /* UTF-8 decode by hand: no Windows headers here */
    for (const unsigned char *u = (const unsigned char *)p; *u && w < 1022;) {
        uint32_t c = *u++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        for (; more && (*u & 0xC0) == 0x80; more--) c = c << 6 | (*u++ & 0x3F);
        if (more) return 0;
        if (c >= 0x10000) {
            c -= 0x10000;
            tmp[w++] = (wchar_t)(0xD800 + (c >> 10));
            tmp[w++] = (wchar_t)(0xDC00 + (c & 0x3FF));
        } else {
            tmp[w++] = c == '/' ? L'\\' : (wchar_t)c;
        }
    }
    tmp[w] = 0;
    /* a drive letter and a colon, or a UNC path: nothing else is a Windows folder */
    int ok = (w >= 2 && tmp[1] == L':' && ((tmp[0] | 32) >= L'a' && (tmp[0] | 32) <= L'z')) ||
             (w >= 3 && tmp[0] == L'\\' && tmp[1] == L'\\');
    if (!ok || (size_t)w >= cap) return 0;
    if (w == 2) tmp[w++] = L'\\', tmp[w] = 0; /* C: means the drive's current folder elsewhere; C:\ here */
    memcpy(out, tmp, ((size_t)w + 1) * sizeof(wchar_t));
    return 1;
}

int term_osc_cwd(int cmd, const char *s, size_t n, wchar_t *out, size_t cap)
{
    if (cmd == 7) {
        if (n < 7 || memcmp(s, "file://", 7)) return 0;
        s += 7, n -= 7;
        size_t h = 0;
        while (h < n && s[h] != '/') h++; /* the host */
        if (h == n) return 0;
        /* file://host/\\server\share: some shells send UNC paths that way; file:////server too */
        return to_path(s + h, n - h, 1, out, cap);
    }
    if (cmd == 9) {
        if (n < 3 || s[0] != '9' || s[1] != ';') return 0;
        s += 2, n -= 2;
        if(n>=4&&!memcmp(s,"wsl:",4))return wsl_osc(s+4,n-4,out,cap);
        if (n >= 2 && s[0] == '"' && s[n - 1] == '"') s++, n -= 2;
        return to_path(s, n, 0, out, cap);
    }
    if (cmd == 1337) {
        if (n < 11 || memcmp(s, "CurrentDir=", 11)) return 0;
        return to_path(s + 11, n - 11, 0, out, cap);
    }
    return 0;
}

static size_t trimmed_len(const wchar_t *s)
{
    size_t n = wcslen(s);
    if (n > 3 && s[n - 1] == L'\\') n--;
    return n;
}

/* PowerShell: 'text', with quotes doubled (it takes the typographic ones as quotes too) */
static int ps_quoted(const wchar_t *head, const wchar_t *s, wchar_t *out, size_t cap)
{
    size_t k = 0;
    for (const wchar_t *p = head; *p; p++) {
        if (k + 1 >= cap) return 0;
        out[k++] = *p;
    }
    for (const wchar_t *p = s; *p; p++) {
        if (k + 3 >= cap) return 0;
        if (*p == L'\'' || *p == 0x2018 || *p == 0x2019 || *p == 0x201A || *p == 0x201B) out[k++] = *p;
        out[k++] = *p;
    }
    out[k++] = L'\'';
    out[k] = 0;
    return 1;
}

int term_cd_command(int kind, const wchar_t *from, const wchar_t *dir, wchar_t *out, size_t cap)
{
    if(kind==TERM_SH_WSL)return dir&&dir[0]&&bash_cd(from,dir,out,cap);
    if (!dir || !dir[0] || (kind != TERM_SH_CMD && kind != TERM_SH_PWSH)) return 0;
    for (const wchar_t *p = dir; *p; p++)
        if (*p < 32 || *p == L'"') return 0;
    if (kind == TERM_SH_CMD && wcschr(dir, L'%')) return 0; /* expanded even in quotes */
    /* short when it can be: the parent is "..", a folder inside is its relative path */
    size_t nf = from ? trimmed_len(from) : 0, nd = trimmed_len(dir);
    if (nf && nd < nf && !_wcsnicmp(from, dir, nd) && (from[nd] == L'\\' || dir[nd - 1] == L'\\') &&
        !wcschr(from + nd + (from[nd] == L'\\'), L'\\')) {
        int n = swprintf(out, cap, L"cd ..");
        return n > 0 && (size_t)n < cap;
    }
    if (nf && nd > nf && !_wcsnicmp(from, dir, nf) && (dir[nf] == L'\\' || from[nf - 1] == L'\\')) {
        const wchar_t *rel = dir + nf + (dir[nf] == L'\\');
        if (kind == TERM_SH_CMD) {
            int n = swprintf(out, cap, L"cd \"%ls\"", rel);
            return n > 0 && (size_t)n < cap;
        }
        if (!wcspbrk(rel, L"[]*?`")) return ps_quoted(L"cd '", rel, out, cap); /* else -LiteralPath, below */
    }
    if (kind == TERM_SH_CMD) {
        int n = swprintf(out, cap, L"cd /d \"%ls\"", dir);
        return n > 0 && (size_t)n < cap;
    }
    return ps_quoted(L"cd -LiteralPath '", dir, out, cap);
}
int term_cmd_prompt(const wchar_t *user, wchar_t *out, size_t cap)
{
    if (!user || !user[0]) user = L"$P$G";
    int n = swprintf(out, cap, L"$E]133;D$E\\$E]133;A$E\\$E]9;9;$P$E\\%ls$E]133;B$E\\", user);
    return n > 0 && (size_t)n < cap;
}

const wchar_t *term_pwsh_hook(void)
{
    /* [char]27 rather than `e: Windows PowerShell 5.1 has no `e */
    return L"$global:__opPrompt = $function:prompt; "
           L"function global:prompt { $e = [char]27; $b = [char]7; "
           L"$l = $executionContext.SessionState.Path.CurrentLocation; "
           L"$o = \"$e]133;D$b$e]133;A$b\"; "
           L"if ($l.Provider.Name -eq 'FileSystem') { $o += \"$e]9;9;$($l.ProviderPath)$b\" }; "
           L"$o + (& $global:__opPrompt) + \"$e]133;B$b\" }";
}

size_t term_base64_utf16(const wchar_t *s, wchar_t *out, size_t cap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = wcslen(s) * 2, k = 0;
    if ((n + 2) / 3 * 4 + 1 > cap) return 0;
    const unsigned char *b = (const unsigned char *)s; /* wchar_t is UTF-16LE here */
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        out[k++] = (wchar_t)tab[v >> 18 & 63];
        out[k++] = (wchar_t)tab[v >> 12 & 63];
        out[k++] = i + 1 < n ? (wchar_t)tab[v >> 6 & 63] : L'=';
        out[k++] = i + 2 < n ? (wchar_t)tab[v & 63] : L'=';
    }
    out[k] = 0;
    return k;
}
