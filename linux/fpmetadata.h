/* Bounded Linux metadata snapshots, shared by local copies and the wire importer.
 * No libacl dependency: Linux POSIX ACL xattrs retain their kernel ABI bytes.
 * Unsupported visible namespaces fail at source; destination security labels
 * remain under their filesystem's policy. All functions return errno values. */
#ifndef FP_METADATA_H
#define FP_METADATA_H
#include <sys/xattr.h>
#define FP_META_MAX 65536u
#define FP_META_ACCESS "system.posix_acl_access"
#define FP_META_DEFAULT "system.posix_acl_default"
typedef struct fp_meta {uint8_t *data;uint32_t size;} fp_meta;
static void fp_meta_free(fp_meta *m){free(m->data);m->data=NULL;m->size=0;}
static int fp_meta_supported(const char *name)
{return (!strncmp(name,"user.",5)&&name[5])||!strcmp(name,FP_META_ACCESS)||!strcmp(name,FP_META_DEFAULT);}
static int fp_meta_names(const void *a,const void *b){return strcmp(*(const char *const *)a,*(const char *const *)b);}
static int fp_meta_acl_valid(const uint8_t *data,uint32_t size)
{
    uint32_t version;if(size<28||(size-4)%8)return 0;memcpy(&version,data,4);if(version!=2)return 0;
    int state=0,named=0,mask=0;uint32_t last=0;int have_id=0;
    for(uint32_t at=4;at<size;at+=8){
        uint16_t tag,perm;uint32_t id;memcpy(&tag,data+at,2);memcpy(&perm,data+at+2,2);memcpy(&id,data+at+4,4);
        if(perm>7)return 0;
        if(!state){if(tag!=1||id!=UINT32_MAX)return 0;state=1;continue;}
        if(state==1&&tag==2){if(id==UINT32_MAX||(have_id&&id<=last))return 0;have_id=1;last=id;named=1;continue;}
        if(state==1&&tag==4){if(id!=UINT32_MAX)return 0;state=2;have_id=0;continue;}
        if(state==2&&tag==8){if(id==UINT32_MAX||(have_id&&id<=last))return 0;have_id=1;last=id;named=1;continue;}
        if(state==2&&tag==16){if(id!=UINT32_MAX)return 0;state=3;mask=1;continue;}
        if((state==2||state==3)&&tag==32&&id==UINT32_MAX&&at+8==size)return !named||mask;
        return 0;
    }
    return 0;
}
static int fp_meta_valid(const uint8_t *data,uint32_t size,int directory)
{
    if(size>FP_META_MAX)return E2BIG;const char *previous=NULL;
    for(uint32_t at=0;at<size;){
        uint32_t n,v;if(size-at<8)return EINVAL;memcpy(&n,data+at,4);memcpy(&v,data+at+4,4);at+=8;
        if(n<2||n>256||n>size-at||v>size-at-n)return EINVAL;
        const char *name=(const char *)data+at;
        if(name[n-1]||memchr(name,0,n-1)||!fp_meta_supported(name)||(previous&&strcmp(previous,name)>=0))return EINVAL;
        if(!strcmp(name,FP_META_DEFAULT)&&!directory)return EINVAL;
        if(!strcmp(name,FP_META_ACCESS)||!strcmp(name,FP_META_DEFAULT)){
            if(!fp_meta_acl_valid(data+at+n,v))return EINVAL;
        }
        previous=name;at+=n+v;
    }
    return 0;
}
static int fp_meta_read(int fd,const char *link,int strict,fp_meta *out)
{
    out->data=NULL;out->size=0;
    ssize_t length=link?llistxattr(link,NULL,0):flistxattr(fd,NULL,0);if(length<0)return errno;
    if(length>FP_META_MAX)return E2BIG;if(!length)return 0;
    char *list=malloc((size_t)length);char **names=malloc((size_t)length*sizeof *names);uint8_t *data=malloc(FP_META_MAX);
    if(!list||!names||!data){free(list);free(names);free(data);return ENOMEM;}
    ssize_t got=link?llistxattr(link,list,(size_t)length):flistxattr(fd,list,(size_t)length);int error=0;uint32_t count=0,bytes=0;
    if(got<0)error=errno;else if(got!=length)error=ESTALE;
    for(size_t at=0;!error&&at<(size_t)got;){
        char *end=memchr(list+at,0,(size_t)got-at);if(!end||end==list+at){error=EIO;break;}
        if(fp_meta_supported(list+at))names[count++]=list+at;else if(strict)error=ENOTSUP;
        at=(size_t)(end-list)+1;
    }
    if(!error)qsort(names,count,sizeof *names,fp_meta_names);
    for(uint32_t i=0;!error&&i<count;i++){
        uint32_t n=(uint32_t)strlen(names[i])+1;ssize_t v=link?lgetxattr(link,names[i],NULL,0):fgetxattr(fd,names[i],NULL,0);
        if(v<0){error=errno;break;}
        if(n>256||bytes>FP_META_MAX-8||n>FP_META_MAX-bytes-8||(uint64_t)v>FP_META_MAX-bytes-8-n){error=E2BIG;break;}
        uint32_t value=(uint32_t)v;memcpy(data+bytes,&n,4);memcpy(data+bytes+4,&value,4);memcpy(data+bytes+8,names[i],n);
        ssize_t read=link?lgetxattr(link,names[i],data+bytes+8+n,value):fgetxattr(fd,names[i],data+bytes+8+n,value);
        if(read<0)error=errno;else if(read!=v)error=ESTALE;else bytes+=8+n+value;
    }
    free(list);free(names);
    if(error){free(data);return error;}if(!bytes){free(data);return 0;}out->data=data;out->size=bytes;return 0;
}
static int fp_meta_fd(int fd,fp_meta *out){return fp_meta_read(fd,NULL,1,out);}
static int fp_meta_link(int parent,const char *name,fp_meta *out)
{
    size_t size=strlen(name)+64;char *path=malloc(size);if(!path)return ENOMEM;
    snprintf(path,size,"/proc/self/fd/%d/%s",parent,name);int error=fp_meta_read(-1,path,1,out);free(path);
    if(!error&&out->size){fp_meta_free(out);error=ENOTSUP;}return error;
}
static int fp_meta_equal(const fp_meta *a,const fp_meta *b)
{return a->size==b->size&&(!a->size||!memcmp(a->data,b->data,a->size));}
static int fp_meta_unchanged(int fd,const fp_meta *before)
{
    fp_meta now={0};int error=fp_meta_fd(fd,&now);if(!error&&!fp_meta_equal(before,&now))error=ESTALE;fp_meta_free(&now);return error;
}
static int fp_meta_verify(int fd,const fp_meta *expected,int preparing)
{
    fp_meta actual={0};int error=fp_meta_read(fd,NULL,0,&actual);uint32_t source=0,target=0;
    while(!error&&source<expected->size){
        uint32_t n,v;memcpy(&n,expected->data+source,4);memcpy(&v,expected->data+source+4,4);uint32_t size=8+n+v;
        if(!(preparing&&!strcmp((const char *)expected->data+source+8,FP_META_ACCESS))){
            if(size>actual.size-target||memcmp(expected->data+source,actual.data+target,size))error=EIO;else target+=size;
        }
        source+=size;
    }
    if(!error&&target!=actual.size)error=EIO;fp_meta_free(&actual);return error;
}
/* New directory preparation retains mode 0700 and sets only user/default
 * metadata before descendants can commit. Final ACL/mode follows at completion.
 * Staged files are private while their full source access policy is applied. */
static int fp_meta_apply(int fd,const fp_meta *source,int preparing)
{
    fp_meta old={0};int error=fp_meta_read(fd,NULL,0,&old);
    for(uint32_t at=0;!error&&at<old.size;){
        uint32_t n,v;memcpy(&n,old.data+at,4);memcpy(&v,old.data+at+4,4);const char *name=(const char *)old.data+at+8;
        if(fremovexattr(fd,name)&&errno!=ENODATA)error=errno;at+=8+n+v;
    }
    fp_meta_free(&old);
    /* Set user attributes while the completed payload still grants its owner
     * write access; source ACLs may deliberately revoke that access. */
    for(int acl=0;!error&&acl<2;acl++)for(uint32_t at=0;!error&&at<source->size;){
        uint32_t n,v;memcpy(&n,source->data+at,4);memcpy(&v,source->data+at+4,4);const char *name=(const char *)source->data+at+8;
        if((!!strncmp(name,"user.",5))==acl&&!(preparing&&!strcmp(name,FP_META_ACCESS))&&fsetxattr(fd,name,source->data+at+8+n,v,0))error=errno;
        at+=8+n+v;
    }
    return error?error:fp_meta_verify(fd,source,preparing);
}
#endif
