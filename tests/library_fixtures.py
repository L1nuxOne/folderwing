"""Owned synthetic catalogues. No copying or editing user libraries."""
import sqlite3,sys
from pathlib import Path
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
def db(name,file,script):
 p=root/name;p.mkdir();d=sqlite3.connect(p/file);d.executescript(script);d.close();return p
c=db('calibre','metadata.db','''
CREATE TABLE library_id(uuid TEXT);INSERT INTO library_id VALUES('synthetic-calibre');
CREATE TABLE books(id INTEGER PRIMARY KEY,title TEXT,author_sort TEXT,path TEXT,pubdate TEXT,timestamp TEXT,has_cover INTEGER,series_index REAL,uuid TEXT);
CREATE TABLE data(id INTEGER PRIMARY KEY,book INTEGER,name TEXT,format TEXT,uncompressed_size INTEGER);
CREATE TABLE authors(id INTEGER,name TEXT);CREATE TABLE books_authors_link(id INTEGER,book INTEGER,author INTEGER);
CREATE TABLE tags(id INTEGER,name TEXT);CREATE TABLE books_tags_link(book INTEGER,tag INTEGER);
CREATE TABLE series(id INTEGER,name TEXT);CREATE TABLE books_series_link(book INTEGER,series INTEGER);
INSERT INTO books VALUES(1,'First book','Fallback','Author/First (1)','2020-01-01','2021-01-01',0,1,'a'),(2,'Zzz escaping path','Fallback','../../escape','2021','2021',0,1,'b'),(3,'No format','Fallback','Author/Third (3)','2022','2022',0,1,'c');
INSERT INTO data VALUES(1,1,'First book','PDF',9000),(2,1,'First book','EPUB',99000),(3,2,'escape','PDF',100);
INSERT INTO authors VALUES(1,'Author One'),(2,'Author Two');INSERT INTO books_authors_link VALUES(1,1,1),(2,1,2);
INSERT INTO tags VALUES(1,'Research');INSERT INTO books_tags_link VALUES(1,1);INSERT INTO series VALUES(1,'Series One');INSERT INTO books_series_link VALUES(1,1);
''')
p=c/'Author/First (1)';p.mkdir(parents=True);(p/'First book.pdf').write_bytes(b'%PDF-1.4\n'+b'0'*8991)
zschema='''
CREATE TABLE items(itemID INTEGER PRIMARY KEY,itemTypeID INTEGER,libraryID INTEGER,key TEXT,dateAdded TEXT);
CREATE TABLE itemTypes(itemTypeID INTEGER,typeName TEXT);CREATE TABLE deletedItems(itemID INTEGER);
CREATE TABLE itemAttachments(itemID INTEGER,parentItemID INTEGER,linkMode INTEGER,contentType TEXT,path TEXT);
CREATE TABLE itemData(itemID INTEGER,fieldID INTEGER,valueID INTEGER);CREATE TABLE fields(fieldID INTEGER,fieldName TEXT);CREATE TABLE itemDataValues(valueID INTEGER,value TEXT);
CREATE TABLE creators(creatorID INTEGER,firstName TEXT,lastName TEXT);CREATE TABLE itemCreators(itemID INTEGER,creatorID INTEGER,orderIndex INTEGER);
CREATE TABLE tags(tagID INTEGER,name TEXT);CREATE TABLE itemTags(itemID INTEGER,tagID INTEGER);
CREATE TABLE collections(collectionID INTEGER,collectionName TEXT,parentCollectionID INTEGER);CREATE TABLE collectionItems(collectionID INTEGER,itemID INTEGER);
'''
z=db('zotero','zotero.sqlite',zschema+'''
INSERT INTO itemTypes VALUES(1,'journalArticle'),(2,'attachment'),(3,'note');
INSERT INTO items VALUES(1,1,1,'REFERENCE','2020'),(2,2,1,'ABCDEFGH','2020'),(3,2,1,'LINKED01','2020'),(4,2,1,'URL00001','2020'),(5,2,1,'STAND001','2020'),(6,1,1,'DELETED1','2020'),(7,3,1,'NOTE0001','2020');
INSERT INTO deletedItems VALUES(6);
INSERT INTO fields VALUES(1,'title'),(2,'date');INSERT INTO itemDataValues VALUES(1,'Research paper'),(2,'2021'),(3,'Standalone PDF');INSERT INTO itemData VALUES(1,1,1),(1,2,2),(5,1,3);
INSERT INTO creators VALUES(1,'Ada','Reader');INSERT INTO itemCreators VALUES(1,1,0);
INSERT INTO itemAttachments VALUES(2,1,0,'application/pdf','storage:paper.pdf'),(3,1,2,'application/pdf','C:\\owned\\linked.pdf'),(4,1,3,'application/pdf',NULL),(5,NULL,0,'application/pdf','storage:standalone.pdf');
INSERT INTO tags VALUES(1,'Research');INSERT INTO itemTags VALUES(1,1);
INSERT INTO collections VALUES(10,'Research',NULL),(11,'Reading',10);INSERT INTO collectionItems VALUES(11,1);
''')
p=z/'storage/ABCDEFGH';p.mkdir(parents=True);(p/'paper.pdf').write_bytes((c/'Author/First (1)/First book.pdf').read_bytes())
db('empty','zotero.sqlite',zschema)
print(root)
if '--ui-attachments' in sys.argv[2:]:
 d=sqlite3.connect(z/'zotero.sqlite')
 d.executescript("""
 INSERT INTO items VALUES(8,2,1,'SECOND01','2020');
 INSERT INTO itemAttachments VALUES(8,1,0,'application/pdf','storage:appendix.pdf');
 """)
 d.executescript("INSERT INTO collections VALUES(20,'Research',NULL);INSERT INTO collectionItems VALUES(20,5);")
 d.close()
 d=sqlite3.connect(c/'metadata.db')
 d.executemany('INSERT INTO tags VALUES(?,?)',[(i+100,'Extra tag %03d'%i) for i in range(300)])
 d.executemany('INSERT INTO books_tags_link VALUES(?,?)',[(3,i+100) for i in range(300)])
 d.commit();d.close()
 for key,name,length in [('SECOND01','appendix.pdf',1234),('STAND001','standalone.pdf',8000)]:
  p=z/'storage'/key;p.mkdir(parents=True,exist_ok=True);(p/name).write_bytes(b'%PDF-1.4\n'+b'0'*(length-9))
