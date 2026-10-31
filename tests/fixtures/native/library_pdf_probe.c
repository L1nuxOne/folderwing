/* Harness observer; no shipped PDF test commands or window properties. */
#include "../../../app/pdfview.c"
int library_pdf_opened(const wchar_t *path){return R.opened&&R.npages>0&&R.path&&fpe_path_equal(R.path,path);}
