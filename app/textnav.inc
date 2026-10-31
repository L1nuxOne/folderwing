/* Text bookmarks use immutable snapshot character offsets, so wrapping and DPI
 * changes do not turn them into a different line. */
static int textnav_ready(void){return R.loaded&&R.front>=0&&!R.markdown_rendered&&SUCCEEDED(R.err);}
static uint32_t textnav_range(void){return R.info.rows>page_rows()?R.info.rows-page_rows():0;}
static void textnav_load(void){R.nav_count=0;R.rail.error=R.nav_file&&R.nav_key?docmarks_load(R.nav_file,R.nav_key,R.nav_marks,DOCMARK_PER_FILE,&R.nav_count):ERROR_PATH_NOT_FOUND;if(R.rail.error)R.nav_count=0;R.nav_loaded=1;}
static int textnav_current(void){if(!textnav_ready())return -1;for(uint32_t i=0;i<R.nav_count;i++)if(R.nav_marks[i].page<=R.info.chars&&row_for(R.nav_marks[i].page)==R.top)return (int)i;return -1;}
static void textnav_build(void){
 rr_geometry(&R.rail,R.cw,R.ch,head_h());R.nav_targets_count=0;if(!textnav_ready())return;if(!R.nav_loaded)textnav_load();
 uint32_t range=textnav_range();int ticks=range?21:1;
 for(int i=0;i<ticks;i++){reading_target *t=R.nav_targets+R.nav_targets_count++;memset(t,0,sizeof *t);t->ratio=(double)i/20;t->value=(uint32_t)((double)range*i/20+.5);textp_row row=row_at(t->value);swprintf(t->title,160,L"%u%% · Line %u",i*5,row.line);wcscpy(t->detail,L"Source preview");}
 for(uint32_t i=0;i<R.nav_count;i++)if(R.nav_marks[i].page<=R.info.chars){reading_target *t=R.nav_targets+R.nav_targets_count++;memset(t,0,sizeof *t);uint32_t row=row_for(R.nav_marks[i].page);t->ratio=range?fmin(1,(double)row/range):0;t->value=i;t->personal=1;wcscpy(t->title,R.nav_marks[i].title);swprintf(t->detail,80,L"Personal bookmark · Line %u",row_at(row).line);}
}
static void textnav_toggle(void){
 if(!textnav_ready())return;textnav_load();if(R.rail.error){InvalidateRect(R.hwnd,NULL,FALSE);return;}int current=textnav_current();docmark mark={0};
 if(current>=0)mark=R.nav_marks[current];else{mark.page=row_at(R.top).start;wchar_t excerpt[96];uint32_t end=row_at(R.top).end,n=end>mark.page?end-mark.page:0;if(n>70)n=70;for(uint32_t i=0;i<n;i++)excerpt[i]=content()[mark.page+i]<32?L' ':content()[mark.page+i];excerpt[n]=0;swprintf(mark.title,DOCMARK_TITLE,L"Line %u: %ls",row_at(R.top).line,excerpt);}
 DWORD error=docmarks_change(R.nav_file,R.nav_key,&mark,current>=0);if(!error)textnav_load();else R.rail.error=error;InvalidateRect(R.hwnd,NULL,FALSE);
}
static void textnav_jump(int index){if(index<0||index>=R.nav_targets_count)return;reading_target *t=R.nav_targets+index;R.top=t->personal?row_for(R.nav_marks[t->value].page):t->value;view_changed();}
static void textnav_menu(int personal_only){textnav_load();textnav_build();int picked=rr_menu(&R.rail,R.hwnd,R.nav_targets,R.nav_targets_count,personal_only,textnav_current()>=0);if(picked==-1)textnav_toggle();else if(picked>=0)textnav_jump(picked);}
static int textnav_input(HWND hwnd,UINT message,WPARAM w,LPARAM l){
 if(R.markdown_rendered)return 0;if(R.selecting&&!R.rail.drag)return 0;textnav_build();int action=rr_input(&R.rail,hwnd,message,w,l,R.nav_targets,R.nav_targets_count,textnav_ready());
 if(action==RR_TOGGLE)textnav_toggle();else if(action==RR_JUMP)textnav_jump(R.rail.hit);else if(action==RR_SCRUB){R.top=(uint32_t)(R.rail.ratio*textnav_range()+.5);view_changed();}else if(action==RR_MENU)textnav_menu(R.rail.bookmark_hover);
 return action!=RR_NONE;
}
static void textnav_paint(HDC dc){if(R.markdown_rendered)return;textnav_build();rr_paint(&R.rail,dc,R.nav_targets,R.nav_targets_count,textnav_range()?(double)R.top/textnav_range():0,textnav_current()>=0,textnav_ready());}
