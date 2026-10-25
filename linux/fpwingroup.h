/* Bounded Windows group result. Sources remain pinned until the correlated
 * durable group result. Fresh checks are answered only in strict token order. */
static int win_group_validate(win_pending *p)
{
    if(cancel())return ECANCELED;if(WQ.failed)return EIO;
    if(!source_intact(p->parent,p->name,p->in,&p->version,0))return ESTALE;
    fp_meta empty={0};return fp_meta_unchanged(p->in,&empty);
}
static int win_prepare_group(void)
{
    wslp_group command={.count=WQ.used},result={0};unsigned checks=0;int checked_error[WSLP_PREPARE_MAX]={0};
    for(unsigned i=0;i<WQ.used;i++)command.files[i]=(wslp_file_result){WQ.slots[i].token,win_group_validate(&WQ.slots[i]),0};
    if(!send(WSLP_GROUP_COMMIT,&command,8+WQ.used*sizeof(wslp_file_result)))goto broken;
    for(;;){
        wslp_head h;if(!io(0,&h,sizeof h,0)||h.id!=J.id)goto broken;
        if(h.type==WSLP_JOB_CANCEL&&!h.size){J.cancelled=1;continue;}
        if(h.type==WSLP_GROUP_CHECK&&h.size==sizeof(wslp_file_result)){
            wslp_file_result request;
            if(!io(0,&request,sizeof request,0)||request.pad||request.error||checks>=WQ.used||request.token!=WQ.slots[checks].token)goto broken;
            request.error=checked_error[checks]=win_group_validate(&WQ.slots[checks]);checks++;
            if(!send(WSLP_GROUP_CHECKED,&request,sizeof request))goto broken;
        }else if(h.type==WSLP_GROUP_COMMITTED&&h.size==8+WQ.used*sizeof(wslp_file_result)){
            if(!io(0,&result,h.size,0)||result.count!=WQ.used||result.pad)goto broken;
            for(unsigned i=0;i<WQ.used;i++){
                wslp_file_result *v=&result.files[i];
                if(v->token!=WQ.slots[i].token||v->pad||v->error<0||v->error>4095||(!v->error&&(i>=checks||checked_error[i]||command.files[i].error)))goto broken;
            }
            break;
        }else goto broken;
    }
    for(unsigned i=0;i<WQ.used;i++){
        win_pending *p=&WQ.slots[i];int error=result.files[i].error;
        if(error){WQ.failed=1;if(!J.cancelled||error!=ECANCELED)event(0,error,p->dest);}
        else{J.p.bytes_done+=p->bytes;J.p.items_done++;progress(p->source,0);}
        if(error&&!J.cancelled)J.p.items_done++;win_pending_close(p);
    }
    WQ.used=0;return !WQ.failed;
broken:
    WQ.failed=WQ.broken=1;J.cancelled=1;
    for(unsigned i=0;i<WQ.used;i++)win_pending_close(&WQ.slots[i]);WQ.used=0;return 0;
}
