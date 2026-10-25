/* The real CLI operation parser, with an inert engine submission boundary.
 * No test can copy, move, delete or recycle a filesystem object. */
#include "fpe.h"
#include <stdio.h>
static unsigned passed, failed, creations, submissions, destructions, configurations;
static fpe_op expected_op;
static const wchar_t *const *expected_sources;
static const wchar_t *expected_dest;
static uint32_t expected_count;
static fpe_conflict expected_policy;
#define CHECK(x) do { if(x)passed++;else { failed++;printf("FAIL %d %s\n",__LINE__,#x); } } while(0)
static fpe_engine *inert_create(const fpe_engine_config *config);
static void inert_destroy(fpe_engine *engine);
static void inert_configure(const fpe_wsl_config *config);
static uint64_t inert_submit(fpe_engine *engine,fpe_op op,const wchar_t *const *sources,
    uint32_t count,const wchar_t *dest,fpe_conflict policy,fpe_job_cb cb,void *user);
#define fpe_engine_create inert_create
#define fpe_engine_destroy inert_destroy
#define fpe_wsl_configure inert_configure
#define fpe_job_submit inert_submit
#define wmain embedded_cli_main
#ifndef CLI_SOURCE
#define CLI_SOURCE "../tools/fpe.c"
#endif
#include CLI_SOURCE
#undef wmain
#undef fpe_job_submit
#undef fpe_wsl_configure
#undef fpe_engine_destroy
#undef fpe_engine_create
static fpe_engine *inert_create(const fpe_engine_config *config)
{ CHECK(!config);creations++;return (fpe_engine *)(uintptr_t)1; }
static void inert_destroy(fpe_engine *engine)
{ CHECK(engine==(fpe_engine *)(uintptr_t)1);destructions++; }
static void inert_configure(const fpe_wsl_config *config)
{ CHECK(config&&!config->enabled&&!config->as_root);configurations++; }
static uint64_t inert_submit(fpe_engine *engine,fpe_op op,const wchar_t *const *sources,
    uint32_t count,const wchar_t *dest,fpe_conflict policy,fpe_job_cb cb,void *user)
{
    submissions++;CHECK(engine==(fpe_engine *)(uintptr_t)1&&op==expected_op);
    CHECK(count==expected_count&&policy==expected_policy);
    CHECK(dest==expected_dest);
    for(uint32_t i=0;i<count&&i<expected_count;i++)CHECK(sources[i]==expected_sources[i]);
    CHECK(cb==on_job&&!user);
    g_final=(fpe_job_progress){.state=FPE_JOB_DONE};
    CHECK(SetEvent(g_job_done));
    return 1;
}
static wchar_t operands[257][32];
static wchar_t *arguments[270];
static const wchar_t *sources[257];
static void reset(fpe_op op,unsigned count)
{
    creations=submissions=destructions=configurations=0;
    g_job_done=g_quit=NULL;memset(&g_final,0,sizeof g_final);
    for(unsigned i=0;i<257;i++){
        swprintf(operands[i],32,L"operand-%u",i);
        arguments[i]=operands[i];sources[i]=operands[i];
    }
    expected_op=op;expected_sources=sources;expected_policy=FPE_CONFLICT_RENAME;
    int transfer=op==FPE_OP_COPY||op==FPE_OP_MOVE;
    expected_count=count-(transfer?1:0);expected_dest=transfer?sources[count-1]:NULL;
}
static void finish(void)
{
    if(g_job_done){CHECK(CloseHandle(g_job_done));g_job_done=NULL;}
    if(g_quit){CHECK(CloseHandle(g_quit));g_quit=NULL;}
    if(creations)SetConsoleCtrlHandler(on_ctrl_job,FALSE);
}
static void test_cli_operand_overflow_refused_without_submission(void)
{
    for(fpe_op op=FPE_OP_COPY;op<=FPE_OP_RECYCLE;op++){
        reset(op,257);
        CHECK(cmd_op(op,257,arguments)==2);
        CHECK(!creations&&!submissions&&!destructions&&!g_job_done&&!g_quit);
        finish();
    }
}
static void test_cli_operand_capacity_preserves_request(void)
{
    for(fpe_op op=FPE_OP_COPY;op<=FPE_OP_RECYCLE;op++){
        reset(op,256);
        CHECK(cmd_op(op,256,arguments)==0);
        CHECK(creations==1&&submissions==1&&destructions==1&&!configurations);
        finish();
    }
}
static void test_cli_interleaved_options_do_not_count_as_operands(void)
{
    for(fpe_op op=FPE_OP_COPY;op<=FPE_OP_RECYCLE;op++){
        for(unsigned count=256;count<=257;count++){
            reset(op,count);int at=0;
            for(unsigned i=0;i<count;i++){
                if(i==0)arguments[at++]=L"--skip";
                if(i==64)arguments[at++]=L"--9p";
                if(i==128)arguments[at++]=L"--overwrite";
                arguments[at++]=operands[i];
            }
            arguments[at++]=L"--fail";expected_policy=FPE_CONFLICT_FAIL;
            CHECK(cmd_op(op,at,arguments)==(count==256?0:2));
            CHECK(configurations==1);
            CHECK(creations==(count==256)&&submissions==(count==256)&&destructions==(count==256));
            finish();
        }
    }
}
int wmain(void)
{
    g_out=GetStdHandle(STD_OUTPUT_HANDLE);g_console=0;
    typedef void (*test_fn)(void);
    test_fn tests[]={test_cli_operand_overflow_refused_without_submission,
        test_cli_operand_capacity_preserves_request,test_cli_interleaved_options_do_not_count_as_operands};
    const char *names[]={"test_cli_operand_overflow_refused_without_submission",
        "test_cli_operand_capacity_preserves_request","test_cli_interleaved_options_do_not_count_as_operands"};
    for(unsigned i=0;i<sizeof tests/sizeof *tests;i++){
        unsigned before=failed;uint64_t start=GetTickCount64();tests[i]();
        printf("CASE %s %s %llu\n",names[i],failed==before?"passed":"failed",(unsigned long long)(GetTickCount64()-start));
    }
    printf("%u passed, %u failed; 3 CLI selectors\n",passed,failed);return failed?1:0;
}
