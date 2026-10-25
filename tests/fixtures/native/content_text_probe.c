/* Harness-only observer of the ordinary text reader; no shipped test hooks. */
#include "../../../app/textview.c"
int content_test_selected(uint32_t line,const wchar_t *expected){if(!R.loaded||R.anchor>=R.caret)return 0;uint32_t actual=1;const wchar_t *text=content();for(uint32_t i=0;i<R.anchor;i++)if(text[i]=='\n')actual++;return actual==line&&R.caret-R.anchor==wcslen(expected)&&!wcsncmp(text+R.anchor,expected,R.caret-R.anchor)&&row_for(R.anchor)>=R.top&&row_for(R.anchor)<R.top+page_rows();}
int content_test_ready_without_selection(void){return R.loaded&&!R.jump_doc&&R.anchor==R.caret;}
