/*
	Description: interface to git programs

	Author: Marco Costalba (C) 2005-2007

	Copyright: See COPYING file that comes with this distribution

*/
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QImageReader>
#include <QRegExp>
#include <QSet>
#include <QSettings>
#include <QTextCodec>
#include <QTextDocument>
#include <QTextStream>

#include <grantlee_templates.h>

#include "cache.h"
#include "git.h"
#include "lanes.h"
#include "myprocess.h"
#include "filehistory.h"
#include "diff/diff.h"

using namespace QGit;

bool Git::TreeEntry::operator<(const TreeEntry& te) const {

	if (this->type == te.type)
		return (this->name < te.name);

	// directories are smaller then files
	// to appear as first when sorted
	if (this->type == "tree")
		return true;

	if (te.type == "tree")
		return false;

	return (this->name < te.name);
}

Git::Git(QObject* p) : QObject(p) {

	EM_INIT(exGitStopped, "Stopping connection with git");

	fileCacheAccessed = cacheNeedsUpdate = isMergeHead = false;
	isStGIT = isGIT = loadingUnAppliedPatches = isTextHighlighterFound = false;
	errorReportingEnabled = true; // report errors if run() fails
	curDomain = NULL;
	revData = NULL;
	revsFiles.reserve(MAX_DICT_SIZE);

    //initialize template engine
    //will load templates from resources under the path /templates
    engine = new Grantlee::Engine();
    Grantlee::FileSystemTemplateLoader::Ptr loader = Grantlee::FileSystemTemplateLoader::Ptr( new Grantlee::FileSystemTemplateLoader() );
    loader->setTemplateDirs(QStringList() << ":/templates/");
    engine->addTemplateLoader(loader);
}

Git::~Git() {
    delete engine;
}

void Git::checkEnvironment() {

	QString version;
	if (run("git --version", &version)) {

		version = version.section(' ', -1, -1).section('.', 0, 2);
		if (version < GIT_VERSION) {

			// simply send information, the 'not compatible version'
			// policy should be implemented upstream
			const QString cmd("Current git version is " + version +
			      " but is required " + GIT_VERSION + " or better");

			const QString errorDesc("Your installed git is too old."
			      "\nPlease upgrade to avoid possible misbehaviours.");

			MainExecErrorEvent* e = new MainExecErrorEvent(cmd, errorDesc);
			QApplication::postEvent(parent(), e);
		}
	} else {
		dbs("Cannot find git files");
		return;
	}
	errorReportingEnabled = false;
	isTextHighlighterFound = run("source-highlight -V", &version);
	errorReportingEnabled = true;
	if (isTextHighlighterFound)
		textHighlighterVersionFound = version.section('\n', 0, 0);
	else
		textHighlighterVersionFound = "GNU source-highlight not installed";
}

void Git::userInfo(SList info) {
/*
  git looks for commit user information in following order:

	- GIT_AUTHOR_NAME and GIT_AUTHOR_EMAIL environment variables
	- repository config file
	- global config file
	- your name, hostname and domain
*/
	const QString env(QProcess::systemEnvironment().join(","));
	QString user(env.section("GIT_AUTHOR_NAME", 1).section(",", 0, 0).section("=", 1).trimmed());
	QString email(env.section("GIT_AUTHOR_EMAIL", 1).section(",", 0, 0).section("=", 1).trimmed());

	info.clear();
	info << "Environment" << user << email;

	errorReportingEnabled = false; // 'git config' could fail, see docs

	run("git config user.name", &user);
	run("git config user.email", &email);
	info << "Local config" << user << email;

	run("git config --global user.name", &user);
	run("git config --global user.email", &email);
	info << "Global config" << user << email;

	errorReportingEnabled = true;
}

bool Git::isImageFile(SCRef file) {

	const QString ext(file.section('.', -1).toLower());
    return QImageReader::supportedImageFormats().contains(ext.toLatin1());
}

bool Git::isBinaryFile(SCRef file) {

	static const char* binaryFileExtensions[] = {"bmp", "gif", "jpeg", "jpg",
	                   "png", "svg", "tiff", "pcx", "xcf", "xpm",
	                   "bz", "bz2", "rar", "tar", "z", "gz", "tgz", "zip", 0};

	if (isImageFile(file))
		return true;

	const QString ext(file.section('.', -1).toLower());
	int i = 0;
	while (binaryFileExtensions[i] != 0)
		if (ext == binaryFileExtensions[i++])
			return true;
	return false;
}

void Git::setThrowOnStop(bool b) {

	if (b)
		EM_REGISTER(exGitStopped);
	else
		EM_REMOVE(exGitStopped);
}

bool Git::isThrowOnStopRaised(int excpId, SCRef curContext) {

	return EM_MATCH(excpId, exGitStopped, curContext);
}

void Git::setTextCodec(QTextCodec* tc) {

    //FIXME: QTextCodec::setCodecForXXX are deprecated, we will have to do the conversion on our own
    //QTextCodec::setCodecForCStrings(tc); // works also with tc == 0 (Latin1)
    //QTextCodec::setCodecForLocale(tc);
	QString name(tc ? tc->name() : "Latin1");

	// workaround Qt issue of mime name different from
	// standard http://www.iana.org/assignments/character-sets
	if (name == "Big5-HKSCS")
		name = "Big5";

    run("git config i18n.commitencoding " + name);
}

QTextCodec* Git::getTextCodec(bool* isGitArchive) {

	*isGitArchive = isGIT;
	if (!isGIT) // can be called also when not in an archive
		return NULL;

	QString runOutput;
    if (!run("git config --get i18n.commitencoding", &runOutput))
		return NULL;

	if (runOutput.isEmpty()) // git docs says default is utf-8
		return QTextCodec::codecForName(QByteArray("utf8"));

	return QTextCodec::codecForName(runOutput.trimmed().toLatin1());
}

const QString Git::quote(SCRef nm) {

	return (QUOTE_CHAR + nm + QUOTE_CHAR);
}

const QString Git::quote(SCList sl) {

	QString q(sl.join(QUOTE_CHAR + ' ' + QUOTE_CHAR));
	q.prepend(QUOTE_CHAR).append(QUOTE_CHAR);
	return q;
}

uint Git::checkRef(const ShaString& sha, uint mask) const {

	RefMap::const_iterator it(refsShaMap.constFind(sha));
	return (it != refsShaMap.constEnd() ? (*it).type & mask : 0);
}

uint Git::checkRef(SCRef sha, uint mask) const {

	RefMap::const_iterator it(refsShaMap.constFind(toTempSha(sha)));
	return (it != refsShaMap.constEnd() ? (*it).type & mask : 0);
}

const QStringList Git::getRefName(SCRef sha, RefType type, QString* curBranch) const {

	if (!checkRef(sha, type))
		return QStringList();

	const Reference& rf = refsShaMap[toTempSha(sha)];

	if (curBranch)
		*curBranch = rf.currentBranch;

	if (type == TAG)
		return rf.tags;

	else if (type == BRANCH)
		return rf.branches;

	else if (type == RMT_BRANCH)
		return rf.remoteBranches;

	else if (type == REF)
		return rf.refs;

	else if (type == APPLIED || type == UN_APPLIED)
		return QStringList(rf.stgitPatch);

	return QStringList();
}

const QStringList Git::getAllRefSha(uint mask) {

	QStringList shas;
	FOREACH (RefMap, it, refsShaMap)
		if ((*it).type & mask)
			shas.append(it.key());
	return shas;
}

const QString Git::getRefSha(SCRef refName, RefType type, bool askGit) {

	bool any = (type == ANY_REF);

	FOREACH (RefMap, it, refsShaMap) {

	        const Reference& rf = *it;

		if ((any || type == TAG) && rf.tags.contains(refName))
			return it.key();

		else if ((any || type == BRANCH) && rf.branches.contains(refName))
			return it.key();

		else if ((any || type == RMT_BRANCH) && rf.remoteBranches.contains(refName))
			return it.key();

		else if ((any || type == REF) && rf.refs.contains(refName))
			return it.key();

		else if ((any || type == APPLIED || type == UN_APPLIED) && rf.stgitPatch == refName)
			return it.key();
	}
	if (!askGit)
		return "";

	// if a ref was not found perhaps is an abbreviated form
	QString runOutput;
	errorReportingEnabled = false;
	bool ok = run("git rev-parse --revs-only " + refName, &runOutput);
	errorReportingEnabled = true;
	return (ok ? runOutput.trimmed() : "");
}

void Git::appendNamesWithId(QStringList& names, SCRef sha, SCList data, bool onlyLoaded) {

	const Rev* r = revLookup(sha);
	if (onlyLoaded && !r)
		return;

	if (onlyLoaded) { // prepare for later sorting
		SCRef cap = QString("%1 ").arg(r->orderIdx, 6);
		FOREACH_SL (it, data)
			names.append(cap + *it);
	} else
		names += data;
}

const QStringList Git::getAllRefNames(uint mask, bool onlyLoaded) {
// returns reference names sorted by loading order if 'onlyLoaded' is set

	QStringList names;
	FOREACH (RefMap, it, refsShaMap) {

		if (mask & TAG)
			appendNamesWithId(names, it.key(), (*it).tags, onlyLoaded);

		if (mask & BRANCH)
			appendNamesWithId(names, it.key(), (*it).branches, onlyLoaded);

		if (mask & RMT_BRANCH)
			appendNamesWithId(names, it.key(), (*it).remoteBranches, onlyLoaded);

		if (mask & REF)
			appendNamesWithId(names, it.key(), (*it).refs, onlyLoaded);

		if ((mask & (APPLIED | UN_APPLIED)) && !onlyLoaded)
			names.append((*it).stgitPatch); // doesn't work with 'onlyLoaded'
        }
        if (onlyLoaded) {
		names.sort();
		QStringList::iterator itN(names.begin());
		for ( ; itN != names.end(); ++itN) // strip 'idx'
			(*itN) = (*itN).section(' ', -1, -1);
	}
	return names;
}

const QString Git::getRevInfo(SCRef sha) {

	if (sha.isEmpty())
		return "";

	uint type = checkRef(sha);
	if (type == 0)
		return "";

	QString refsInfo;
	if (type & BRANCH) {
		const QString cap(type & CUR_BRANCH ? "HEAD: " : "Branch: ");
		refsInfo =  cap + getRefName(sha, BRANCH).join(" ");
	}
	if (type & RMT_BRANCH)
		refsInfo.append("   Remote branch: " + getRefName(sha, RMT_BRANCH).join(" "));

	if (type & TAG)
		refsInfo.append("   Tag: " + getRefName(sha, TAG).join(" "));

	if (type & REF)
		refsInfo.append("   Ref: " + getRefName(sha, REF).join(" "));

	if (type & APPLIED)
		refsInfo.append("   Patch: " + getRefName(sha, APPLIED).join(" "));

	if (type & UN_APPLIED)
		refsInfo.append("   Patch: " + getRefName(sha, UN_APPLIED).join(" "));

	if (type & TAG) {
		SCRef msg(getTagMsg(sha));
		if (!msg.isEmpty())
			refsInfo.append("  [" + msg + "]");
	}
	return refsInfo.trimmed();
}

const QString Git::getTagMsg(SCRef sha) {

	if (!checkRef(sha, TAG)) {
		dbs("ASSERT in Git::getTagMsg, tag not found");
		return "";
	}
	Reference& rf = refsShaMap[toTempSha(sha)];

	if (!rf.tagMsg.isEmpty())
		return rf.tagMsg;

	QRegExp pgp("-----BEGIN PGP SIGNATURE*END PGP SIGNATURE-----",
	            Qt::CaseSensitive, QRegExp::Wildcard);

	if (!rf.tagObj.isEmpty()) {
		QString ro;
		if (run("git cat-file tag " + rf.tagObj, &ro))
			rf.tagMsg = ro.section("\n\n", 1).remove(pgp).trimmed();
	}
	return rf.tagMsg;
}

bool Git::isPatchName(SCRef nm) {

	if (!getRefSha(nm, UN_APPLIED, false).isEmpty())
		return true;

	return !getRefSha(nm, APPLIED, false).isEmpty();
}

void Git::addExtraFileInfo(QString* rowName, SCRef sha, SCRef diffToSha, bool allMergeFiles) {

	const RevFile* files = getFiles(sha, diffToSha, allMergeFiles);
	if (!files)
		return;

	int idx = findFileIndex(*files, *rowName);
	if (idx == -1)
		return;

	QString extSt(files->extendedStatus(idx));
	if (extSt.isEmpty())
		return;

	*rowName = extSt;
}

void Git::removeExtraFileInfo(QString* rowName) {

	if (rowName->contains(" --> ")) // return destination file name
		*rowName = rowName->section(" --> ", 1, 1).section(" (", 0, 0);
}

void Git::formatPatchFileHeader(QString* rowName, SCRef sha, SCRef diffToSha,
                                bool combined, bool allMergeFiles) {
	if (combined) {
		rowName->prepend("diff --combined ");
		return; // TODO rename/copy still not supported in this case
	}
	// let's see if it's a rename/copy...
	addExtraFileInfo(rowName, sha, diffToSha, allMergeFiles);

	if (rowName->contains(" --> ")) { // ...it is!

		SCRef destFile(rowName->section(" --> ", 1, 1).section(" (", 0, 0));
		SCRef origFile(rowName->section(" --> ", 0, 0));
		*rowName = "diff --git a/" + origFile + " b/" + destFile;
	} else
		*rowName = "diff --git a/" + *rowName + " b/" + *rowName;
}

void Git::cancelDataLoading(const FileHistory* fh) {
// normally called when closing file viewer

	emit cancelLoading(fh); // non blocking
}

const Rev* Git::revLookup(SCRef sha, const FileHistory* fh) const {

	return revLookup(toTempSha(sha), fh);
}

const Rev* Git::revLookup(const ShaString& sha, const FileHistory* fh) const {

	const RevMap& r = (fh ? fh->revs : revData->revs);
	return (sha.latin1() ? r.value(sha) : NULL);
}

bool Git::run(SCRef runCmd, QString* runOutput, QObject* receiver, SCRef buf) {

	QByteArray ba;
	bool ret = run(runOutput ? &ba : NULL, runCmd, receiver, buf);
	if (runOutput)
		*runOutput = ba;

	return ret;
}

bool Git::run(QByteArray* runOutput, SCRef runCmd, QObject* receiver, SCRef buf) {

	MyProcess p(parent(), this, workDir, errorReportingEnabled);
	return p.runSync(runCmd, runOutput, receiver, buf);
}

MyProcess* Git::runAsync(SCRef runCmd, QObject* receiver, SCRef buf) {

	MyProcess* p = new MyProcess(parent(), this, workDir, errorReportingEnabled);
	if (!p->runAsync(runCmd, receiver, buf)) {
		delete p;
		p = NULL;
	}
	return p; // auto-deleted when done
}

MyProcess* Git::runAsScript(SCRef runCmd, QObject* receiver, SCRef buf) {

	const QString scriptFile(workDir + "/qgit_script" + QGit::SCRIPT_EXT);
#ifndef Q_OS_WIN32
	// without this process doesn't start under Linux
	QString cmd(runCmd.startsWith("#!") ? runCmd : "#!/bin/sh\n" + runCmd);
#else
	QString cmd(runCmd);
#endif
	if (!writeToFile(scriptFile, cmd, true))
		return NULL;

	MyProcess* p = runAsync(scriptFile, receiver, buf);
	if (p)
		connect(p, SIGNAL(eof()), this, SLOT(on_runAsScript_eof()));
	return p;
}

void Git::on_runAsScript_eof() {

	QDir dir(workDir);
	dir.remove("qgit_script" + QGit::SCRIPT_EXT);
}

void Git::cancelProcess(MyProcess* p) {

	if (p)
		p->on_cancel(); // non blocking call
}

int Git::findFileIndex(const RevFile& rf, SCRef name) {

	if (name.isEmpty())
		return -1;

	int idx = name.lastIndexOf('/') + 1;
	SCRef dr = name.left(idx);
	SCRef nm = name.mid(idx);

	for (uint i = 0, cnt = rf.count(); i < cnt; ++i) {
		if (fileNamesVec[rf.nameAt(i)] == nm && dirNamesVec[rf.dirAt(i)] == dr)
			return i;
	}
	return -1;
}

const QString Git::getLaneParent(SCRef fromSHA, int laneNum) {

	const Rev* rs = revLookup(fromSHA);
	if (!rs)
		return "";

	for (int idx = rs->orderIdx - 1; idx >= 0; idx--) {

		const Rev* r = revLookup(revData->revOrder[idx]);
		if (laneNum >= r->lanes.count())
			return "";

		if (!isFreeLane(r->lanes[laneNum])) {

			int type = r->lanes[laneNum], parNum = 0;
			while (!isMerge(type) && type != ACTIVE) {

				if (isHead(type))
					parNum++;

				type = r->lanes[--laneNum];
			}
			return r->parent(parNum);
		}
	}
	return "";
}

const QStringList Git::getChilds(SCRef parent) {

	QStringList childs;
	const Rev* r = revLookup(parent);
	if (!r)
		return childs;

	for (int i = 0; i < r->childs.count(); i++)
		childs.append(revData->revOrder[r->childs[i]]);

	// reorder childs by loading order
	QStringList::iterator itC(childs.begin());
	for ( ; itC != childs.end(); ++itC) {
		const Rev* r = revLookup(*itC);
		(*itC).prepend(QString("%1 ").arg(r->orderIdx, 6));
	}
	childs.sort();
	for (itC = childs.begin(); itC != childs.end(); ++itC)
		(*itC) = (*itC).section(' ', -1, -1);

	return childs;
}

const QString Git::getShortLog(SCRef sha) {

	const Rev* r = revLookup(sha);
	return (r ? r->shortLog() : "");
}

MyProcess* Git::getDiff(SCRef sha, QObject* receiver, SCRef diffToSha, bool combined) {

	if (sha.isEmpty())
		return NULL;

	QString runCmd;
	if (sha != ZERO_SHA) {
		runCmd = "git diff-tree --no-color -r --patch-with-stat ";
		runCmd.append(combined ? "-c " : "-C -m "); // TODO rename for combined
		runCmd.append(diffToSha + " " + sha); // diffToSha could be empty
	} else
		runCmd = "git diff-index --no-color -r -m --patch-with-stat HEAD";

	return runAsync(runCmd, receiver);
}

QString Git::getDiff(SCRef sha)
{
    if (sha.isEmpty())
        return "";

    QString runCmd = QString("git diff-tree --find-renames -p %1").arg(sha);
    QString output;
    run(runCmd, &output);

    QStringList lines = output.split(QRegularExpression("\n|(\r\n)"));
    if(lines.size() > 0)
    {
        lines.removeFirst();
        output = lines.join("\n");
    }
    return output;
}

const QString Git::getWorkDirDiff(SCRef fileName) {

	QString runCmd("git diff-index --no-color -r -z -m -p --full-index --no-commit-id HEAD"), runOutput;
	if (!fileName.isEmpty())
		runCmd.append(" -- " + quote(fileName));

	if (!run(runCmd, &runOutput))
		return "";

	/* For unknown reasons file sha of index is not ZERO_SHA but
	   a value of unknown origin.
	   Replace that with ZERO_SHA so to not fool annotate
	*/
	int idx = runOutput.indexOf("..");
	if (idx != -1)
		runOutput.replace(idx + 2, 40, ZERO_SHA);

	return runOutput;
}

const QString Git::getFileSha(SCRef file, SCRef revSha) {

	if (revSha == ZERO_SHA) {
		QStringList files, dummy;
		getWorkDirFiles(files, dummy, RevFile::ANY);
		if (files.contains(file))
			return ZERO_SHA; // it is unknown to git
	}
	const QString sha(revSha == ZERO_SHA ? "HEAD" : revSha);
	QString runCmd("git ls-tree -r " + sha + " " + quote(file)), runOutput;
	if (!run(runCmd, &runOutput))
		return "";

	return runOutput.mid(12, 40); // could be empty, deleted file case
}

MyProcess* Git::getFile(SCRef fileSha, QObject* receiver, QByteArray* result, SCRef fileName) {

	QString runCmd;
	/*
	  symlinks in git are one line files with just the name of the target,
	  not the target content. Instead 'cat' command resolves symlinks and
	  returns target content. So we use 'cat' only if the file is modified
	  in working dir, to let annotation work for changed files, otherwise
	  we go with a safe 'git cat-file blob HEAD' instead.
	  NOTE: This fails if the modified file is a new symlink, converted
	  from an old plain file. In this case annotation will fail until
	  change is committed.
	*/
	if (fileSha == ZERO_SHA)

#ifdef Q_OS_WIN32
    {
		QString winPath = quote(fileName);
		winPath.replace("/", "\\");
		runCmd = "type " + winPath;
    }
#else
		runCmd = "cat " + quote(fileName);
#endif

	else {
		if (fileSha.isEmpty()) // deleted
			runCmd = "git diff-tree HEAD HEAD"; // fake an empty file reading
		else
			runCmd = "git cat-file blob " + fileSha;
	}
	if (!receiver) {
		run(result, runCmd);
		return NULL; // in case of sync call we ignore run() return value
	}
	return runAsync(runCmd, receiver);
}

MyProcess* Git::getHighlightedFile(SCRef fileSha, QObject* receiver, QString* result, SCRef fileName) {

	if (!isTextHighlighter()) {
		dbs("ASSERT in getHighlightedFile: highlighter not found");
		return NULL;
	}
	QString ext(fileName.section('.', -1, -1, QString::SectionIncludeLeadingSep));
	QString inputFile(workDir + "/qgit_hlght_input" + ext);
	if (!saveFile(fileSha, fileName, inputFile))
		return NULL;

	QString runCmd("source-highlight --failsafe -f html -i " + quote(inputFile));

	if (!receiver) {
		run(runCmd, result);
		on_getHighlightedFile_eof();
		return NULL; // in case of sync call we ignore run() return value
	}
	MyProcess* p = runAsync(runCmd, receiver);
	if (p)
		connect(p, SIGNAL(eof()), this, SLOT(on_getHighlightedFile_eof()));
	return p;
}

void Git::on_getHighlightedFile_eof() {

	QDir dir(workDir);
	const QStringList sl(dir.entryList(QStringList() << "qgit_hlght_input*"));
	FOREACH_SL (it, sl)
		dir.remove(*it);
}

bool Git::saveFile(SCRef fileSha, SCRef fileName, SCRef path) {

	QByteArray fileData;
	getFile(fileSha, NULL, &fileData, fileName); // sync call
	if (isBinaryFile(fileName))
		return writeToFile(path, fileData);

	return writeToFile(path, QString(fileData));
}

bool Git::getTree(SCRef treeSha, TreeInfo& ti, bool isWorkingDir, SCRef path) {

	QStringList deleted;
	if (isWorkingDir) {

		// retrieve unknown and deleted files under path
		QStringList unknowns, dummy;
		getWorkDirFiles(unknowns, dummy, RevFile::UNKNOWN);

		FOREACH_SL (it, unknowns) {

			// don't add files under other directories
			QFileInfo f(*it);
			SCRef d(f.dir().path());

			if (d == path || (path.isEmpty() && d == ".")) {
				TreeEntry te(f.fileName(), "", "?");
				ti.append(te);
			}
		}
		getWorkDirFiles(deleted, dummy, RevFile::DELETED);
	}
	// if needed fake a working directory tree starting from HEAD tree
	QString runOutput, tree(treeSha);
	if (treeSha == ZERO_SHA) {
		// HEAD could be empty for just init'ed repositories
		if (!run("git rev-parse --revs-only HEAD", &tree))
			return false;

		tree = tree.trimmed();
	}
	if (!tree.isEmpty() && !run("git ls-tree " + tree, &runOutput))
		return false;

	const QStringList sl(runOutput.split('\n', QString::SkipEmptyParts));
	FOREACH_SL (it, sl) {

		// append any not deleted file
		SCRef fn((*it).section('\t', 1, 1));
		SCRef fp(path.isEmpty() ? fn : path + '/' + fn);

		if (deleted.empty() || (deleted.indexOf(fp) == -1)) {
			TreeEntry te(fn, (*it).mid(12, 40), (*it).mid(7, 4));
			ti.append(te);
		}
	}
	qSort(ti); // list directories before files
	return true;
}

void Git::getWorkDirFiles(SList files, SList dirs, RevFile::StatusFlag status) {

	files.clear();
	dirs.clear();
	const RevFile* f = getFiles(ZERO_SHA);
	if (!f)
		return;

	for (int i = 0; i < f->count(); i++) {

		if (f->statusCmp(i, status)) {

			SCRef fp(filePath(*f, i));
			files.append(fp);
			for (int j = 0, cnt = fp.count('/'); j < cnt; j++) {

				SCRef dir(fp.section('/', 0, j));
				if (dirs.indexOf(dir) == -1)
					dirs.append(dir);
			}
		}
	}
}

bool Git::isNothingToCommit() {

	if (!revsFiles.contains(ZERO_SHA_RAW))
		return true;

	const RevFile* rf = revsFiles[ZERO_SHA_RAW];
	return (rf->count() == workingDirInfo.otherFiles.count());
}

bool Git::isTreeModified(SCRef sha) {

	const RevFile* f = getFiles(sha);
	if (!f)
		return true; // no files info, stay on the safe side

	for (int i = 0; i < f->count(); ++i)
		if (!f->statusCmp(i, RevFile::MODIFIED))
			return true;

	return false;
}

bool Git::isParentOf(SCRef par, SCRef child) {

	const Rev* c = revLookup(child);
	return (c && c->parentsCount() == 1 && QString(c->parent(0)) == par); // no merges
}

bool Git::isSameFiles(SCRef tree1Sha, SCRef tree2Sha) {

	// early skip common case of browsing with up and down arrows, i.e.
	// going from parent(child) to child(parent). In this case we can
	// check RevFileMap and skip a costly 'git diff-tree' call.
	if (isParentOf(tree1Sha, tree2Sha))
		return !isTreeModified(tree2Sha);

	if (isParentOf(tree2Sha, tree1Sha))
		return !isTreeModified(tree1Sha);

	const QString runCmd("git diff-tree --no-color -r " + tree1Sha + " " + tree2Sha);
	QString runOutput;
	if (!run(runCmd, &runOutput))
		return false;

	bool isChanged = (runOutput.indexOf(" A\t") != -1 || runOutput.indexOf(" D\t") != -1);
	return !isChanged;
}

const QStringList Git::getDescendantBranches(SCRef sha, bool shaOnly) {

	QStringList tl;
	const Rev* r = revLookup(sha);
	if (!r || (r->descBrnMaster == -1))
		return tl;

	const QVector<int>& nr = revLookup(revData->revOrder[r->descBrnMaster])->descBranches;

	for (int i = 0; i < nr.count(); i++) {

		const ShaString& sha = revData->revOrder[nr[i]];
		if (shaOnly) {
			tl.append(sha);
			continue;
		}
		SCRef cap = " (" + sha + ") ";
		RefMap::const_iterator it(refsShaMap.find(sha));
		if (it == refsShaMap.constEnd())
			continue;

		if (!(*it).branches.empty())
			tl.append((*it).branches.join(" ").append(cap));

		if (!(*it).remoteBranches.empty())
			tl.append((*it).remoteBranches.join(" ").append(cap));
	}
	return tl;
}

const QStringList Git::getNearTags(bool goDown, SCRef sha) {

	QStringList tl;
	const Rev* r = revLookup(sha);
	if (!r)
		return tl;

	int nearRefsMaster = (goDown ? r->descRefsMaster : r->ancRefsMaster);
	if (nearRefsMaster == -1)
		return tl;

	const QVector<int>& nr = goDown ? revLookup(revData->revOrder[nearRefsMaster])->descRefs :
	                                  revLookup(revData->revOrder[nearRefsMaster])->ancRefs;

	for (int i = 0; i < nr.count(); i++) {

		const ShaString& sha = revData->revOrder[nr[i]];
		SCRef cap = " (" + sha + ")";
		RefMap::const_iterator it(refsShaMap.find(sha));
		if (it != refsShaMap.constEnd())
			tl.append((*it).tags.join(cap).append(cap));
	}
	return tl;
}

const QString Git::getLastCommitMsg() {

	// FIXME: Make sure the amend action is not called when there is
	// nothing to amend. That is in empty repository or over StGit stack
	// with nothing applied.
	QString sha;
	QString top;
	if (run("git rev-parse --verify HEAD", &top))
	    sha = top.trimmed();
	else {
		dbs("ASSERT: getLastCommitMsg head is not valid");
		return "";
	}

	const Rev* c = revLookup(sha);
	if (!c) {
		dbp("ASSERT: getLastCommitMsg sha <%1> not found", sha);
		return "";
	}

	return c->shortLog() + "\n\n" + c->longLog().trimmed();
}

const QString Git::getNewCommitMsg() {

	const Rev* c = revLookup(ZERO_SHA);
	if (!c) {
		dbs("ASSERT: getNewCommitMsg zero_sha not found");
		return "";
	}

	QString status = c->longLog();
	status.prepend('\n').replace(QRegExp("\\n([^#])"), "\n#\\1"); // comment all the lines
	return status;
}

const QString Git::colorMatch(SCRef txt, QRegExp& regExp) {

	QString text;

    text = txt.toHtmlEscaped();

	if (regExp.isEmpty())
		return text;

	SCRef startCol(QString::fromLatin1("<b><font color=\"red\">"));
	SCRef endCol(QString::fromLatin1("</font></b>"));
	int pos = 0;
	while ((pos = text.indexOf(regExp, pos)) != -1) {

		SCRef match(regExp.cap(0));
		const QString coloredText(startCol + match + endCol);
		text.replace(pos, match.length(), coloredText);
		pos += coloredText.length();
	}
	return text;
}

const QString Git::getDesc(SCRef sha, FileHistory* fh) {

	if (sha.isEmpty())
		return "";

	const Rev* c = revLookup(sha, fh);
	if (!c)            // sha of a not loaded revision, as
		return ""; // example asked from file history

	QString text;
	if (c->isDiffCache)
		text = Qt::convertFromPlainText(c->longLog());
	else {
        //render template into text with the help of grantlee
        QVariantHash mapping;

        mapping["TYPE_WRITER_FONT_FAMILY"] = TYPE_WRITER_FONT.family();
        mapping["TYPE_WRITER_FONT_SIZE"] = TYPE_WRITER_FONT.pointSize();

        mapping["committer"] = c->committer();
        mapping["author"] = c->author();
        mapping["author_date"] = getLocalDate(c->authorDate());

        if(c->isApplied || c->isUnApplied) {
            QStringList patches(getRefName(sha, APPLIED));
            patches += getRefName(sha, UN_APPLIED);
            mapping["patches"] = patches;
        }
        else {
            mapping["parents"] = c->parents();
            mapping["children"] = getChilds(sha);
            mapping["branches"] = getDescendantBranches(sha);
            mapping["following"] = getNearTags(!optGoDown, sha);
            mapping["preceding"] = getNearTags(optGoDown, sha);
        }

        mapping["short_log"] = c->shortLog();
        mapping["long_log"] = c->longLog();

        //load diff for this commit
        QString diffText = getDiff(sha);
        auto diff = TreeDiff::createFromString(diffText);
        if(diff)
        {
            mapping["diff_exists"] = true;
            mapping["diff"] = QVariant::fromValue(diff.to_value());
            FileDiff::HunksList d = diff.to_value()->entries()[0]->fileDiff()->hunks();
        }
        else {
            mapping["diff_exists"] = false;
            qWarning() << "error while generating diff for commit " << sha << ": "
                     << diff.to_error().c_str()
                     << "\n";
        }

        Grantlee::Context context(mapping);
        Grantlee::Template t = engine->loadByName("desc.html" );
        text = t->render(&context);
        if(t->error()) {
            text = t->errorString();
        }
	}

	return text;
}

const RevFile* Git::insertNewFiles(SCRef sha, SCRef data) {

	/* we use an independent FileNamesLoader to avoid data
	 * corruption if we are loading file names in background
	 */
	FileNamesLoader fl;

	RevFile* rf = new RevFile();
	parseDiffFormat(*rf, data, fl);
	flushFileNames(fl);

	revsFiles.insert(toPersistentSha(sha, revsFilesShaBackupBuf), rf);
	return rf;
}

bool Git::runDiffTreeWithRenameDetection(SCRef runCmd, QString* runOutput) {
/* Under some cases git could warn out:

      "too many files, skipping inexact rename detection"

   So if this occurs fallback on NO rename detection.
*/
	QString cmd(runCmd); // runCmd must be without -C option
	cmd.replace("git diff-tree", "git diff-tree -C");

	errorReportingEnabled = false;
	bool renameDetectionOk = run(cmd, runOutput);
	errorReportingEnabled = true;

	if (!renameDetectionOk) // retry without rename detection
		return run(runCmd, runOutput);

	return true;
}

const RevFile* Git::getAllMergeFiles(const Rev* r) {

	SCRef mySha(ALL_MERGE_FILES + r->sha());
	if (revsFiles.contains(toTempSha(mySha)))
		return revsFiles[toTempSha(mySha)];

	EM_PROCESS_EVENTS; // 'git diff-tree' could be slow

	QString runCmd("git diff-tree --no-color -r -m " + r->sha()), runOutput;
	if (!runDiffTreeWithRenameDetection(runCmd, &runOutput))
		return NULL;

	return insertNewFiles(mySha, runOutput);
}

const RevFile* Git::getFiles(SCRef sha, SCRef diffToSha, bool allFiles, SCRef path) {

	const Rev* r = revLookup(sha);
	if (!r)
		return NULL;

	if (r->parentsCount() == 0) // skip initial rev
		return NULL;

	if (r->parentsCount() > 1 && diffToSha.isEmpty() && allFiles)
		return getAllMergeFiles(r);

	if (!diffToSha.isEmpty() && (sha != ZERO_SHA)) {

		QString runCmd("git diff-tree --no-color -r -m ");
		runCmd.append(diffToSha + " " + sha);
		if (!path.isEmpty())
			runCmd.append(" " + path);

		EM_PROCESS_EVENTS; // 'git diff-tree' could be slow

		QString runOutput;
		if (!runDiffTreeWithRenameDetection(runCmd, &runOutput))
			return NULL;

		// we insert a dummy revision file object. It will be
		// overwritten at each request but we don't care.
		return insertNewFiles(CUSTOM_SHA, runOutput);
	}
	if (revsFiles.contains(r->sha()))
		return revsFiles[r->sha()]; // ZERO_SHA search arrives here

	if (sha == ZERO_SHA) {
		dbs("ASSERT in Git::getFiles, ZERO_SHA not found");
		return NULL;
	}

	EM_PROCESS_EVENTS; // 'git diff-tree' could be slow

	QString runCmd("git diff-tree --no-color -r -c " + sha), runOutput;
	if (!runDiffTreeWithRenameDetection(runCmd, &runOutput))
		return NULL;

	if (revsFiles.contains(r->sha())) // has been created in the mean time?
		return revsFiles[r->sha()];

	cacheNeedsUpdate = true;
	return insertNewFiles(sha, runOutput);
}

bool Git::startFileHistory(SCRef sha, SCRef startingFileName, FileHistory* fh) {

	QStringList args(getDescendantBranches(sha, true));
	if (args.isEmpty())
		args << "HEAD";

	QString newestFileName = getNewestFileName(args, startingFileName);
	fh->resetFileNames(newestFileName);

	args.clear(); // load history from all the branches
	args << getAllRefSha(BRANCH | RMT_BRANCH);

	args << "--" << newestFileName;
	return startRevList(args, fh);
}

const QString Git::getNewestFileName(SCList branches, SCRef fileName) {

	QString curFileName(fileName), runOutput, args;
	while (true) {
		args = branches.join(" ") + " -- " + curFileName;
		if (!run("git ls-tree " + args, &runOutput))
			break;

		if (!runOutput.isEmpty())
			break;

		QString msg("Retrieving file renames, now at '" + curFileName + "'...");
		QApplication::postEvent(parent(), new MessageEvent(msg));
		EM_PROCESS_EVENTS_NO_INPUT;

		if (!run("git rev-list -n1 " + args, &runOutput))
			break;

		if (runOutput.isEmpty()) // try harder
			if (!run("git rev-list --full-history -n1 " + args, &runOutput))
				break;

		if (runOutput.isEmpty())
			break;

		SCRef sha = runOutput.trimmed();
		QStringList newCur;
		if (!populateRenamedPatches(sha, QStringList(curFileName), NULL, &newCur, true))
			break;

		curFileName = newCur.first();
	}
	return curFileName;
}

void Git::getFileFilter(SCRef path, ShaSet& shaSet) const {

	shaSet.clear();
	QRegExp rx(path, Qt::CaseInsensitive, QRegExp::Wildcard);
	FOREACH (ShaVect, it, revData->revOrder) {

		if (!revsFiles.contains(*it))
			continue;

		// case insensitive, wildcard search
		const RevFile* rf = revsFiles[*it];
		for (int i = 0; i < rf->count(); ++i)
			if (filePath(*rf, i).contains(rx)) {
				shaSet.insert(*it);
				break;
			}
	}
}

bool Git::getPatchFilter(SCRef exp, bool isRegExp, ShaSet& shaSet) {

	shaSet.clear();
	QString buf;
	FOREACH (ShaVect, it, revData->revOrder)
		if (*it != ZERO_SHA_RAW)
			buf.append(*it).append('\n');

	if (buf.isEmpty())
		return true;

	EM_PROCESS_EVENTS; // 'git diff-tree' could be slow

	QString runCmd("git diff-tree --no-color -r -s --stdin "), runOutput;
	if (isRegExp)
		runCmd.append("--pickaxe-regex ");

	runCmd.append(quote("-S" + exp));
	if (!run(runCmd, &runOutput, NULL, buf))
		return false;

	const QStringList sl(runOutput.split('\n', QString::SkipEmptyParts));
	FOREACH_SL (it, sl)
		shaSet.insert(*it);

	return true;
}

bool Git::resetCommits(int parentDepth) {

	QString runCmd("git reset --soft HEAD~");
	runCmd.append(QString::number(parentDepth));
	return run(runCmd);
}

bool Git::applyPatchFile(SCRef patchPath, bool fold, bool isDragDrop) {

	if (isStGIT) {
		if (fold) {
			bool ok = run("stg fold " + quote(patchPath)); // merge in working dir
			if (ok)
				ok = run("stg refresh"); // update top patch
			return ok;
		} else
			return run("stg import --mail " + quote(patchPath));
	}
	QString runCmd("git am --utf8 --3way ");

	QSettings settings;
	const QString APOpt(settings.value(AM_P_OPT_KEY).toString());
	if (!APOpt.isEmpty())
		runCmd.append(APOpt.trimmed() + " ");

	if (isDragDrop)
		runCmd.append("--keep ");

	if (testFlag(SIGN_PATCH_F))
		runCmd.append("--signoff ");

	return run(runCmd + quote(patchPath));
}

const QStringList Git::sortShaListByIndex(SCList shaList) {

	QStringList orderedShaList;
	FOREACH_SL (it, shaList)
		appendNamesWithId(orderedShaList, *it, QStringList(*it), true);

	orderedShaList.sort();
	QStringList::iterator itN(orderedShaList.begin());
	for ( ; itN != orderedShaList.end(); ++itN) // strip 'idx'
		(*itN) = (*itN).section(' ', -1, -1);

        return orderedShaList;
}

bool Git::formatPatch(SCList shaList, SCRef dirPath, SCRef remoteDir) {

	bool remote = !remoteDir.isEmpty();
	QSettings settings;
	const QString FPOpt(settings.value(FMT_P_OPT_KEY).toString());

	QString runCmd("git format-patch --no-color");
	if (testFlag(NUMBERS_F) && !remote)
		runCmd.append(" -n");

	if (remote)
		runCmd.append(" --keep-subject");

	runCmd.append(" -o " + quote(dirPath));
	if (!FPOpt.isEmpty())
		runCmd.append(" " + FPOpt.trimmed());

	const QString tmp(workDir);
	if (remote)
		workDir = remoteDir; // run() uses workDir value

	// shaList is ordered by newest to oldest
	runCmd.append(" " + shaList.last());
	runCmd.append(QString::fromLatin1("^..") + shaList.first());
	bool ret = run(runCmd);
	workDir = tmp;
	return ret;
}

const QStringList Git::getOtherFiles(SCList selFiles, bool onlyInIndex) {

	const RevFile* files = getFiles(ZERO_SHA); // files != NULL
	QStringList notSelFiles;
	for (int i = 0; i < files->count(); ++i) {
		SCRef fp = filePath(*files, i);
		if (selFiles.indexOf(fp) == -1) { // not selected...
			if (!onlyInIndex || files->statusCmp(i, RevFile::IN_INDEX))
				notSelFiles.append(fp);
		}
	}
	return notSelFiles;
}

bool Git::updateIndex(SCList selFiles) {

	const RevFile* files = getFiles(ZERO_SHA); // files != NULL

	QStringList toAdd, toRemove;
	FOREACH_SL (it, selFiles) {
		int idx = findFileIndex(*files, *it);
		if (files->statusCmp(idx, RevFile::DELETED))
			toRemove << *it;
		else
			toAdd << *it;
	}
	if (!toRemove.isEmpty() && !run("git rm --cached --ignore-unmatch -- " + quote(toRemove)))
		return false;

	if (!toAdd.isEmpty() && !run("git add -- " + quote(toAdd)))
		return false;

	return true;
}

bool Git::commitFiles(SCList selFiles, SCRef msg, bool amend) {

	const QString msgFile(gitDir + "/qgit_cmt_msg.txt");
	if (!writeToFile(msgFile, msg)) // early skip
		return false;

	// add user selectable commit options
	QSettings settings;
	const QString CMArgs(settings.value(CMT_ARGS_KEY).toString());

	QString cmtOptions;
	if (!CMArgs.isEmpty())
		cmtOptions.append(" " + CMArgs);

	if (testFlag(SIGN_CMT_F))
		cmtOptions.append(" -s");

	if (testFlag(VERIFY_CMT_F))
		cmtOptions.append(" -v");

	if (amend)
		cmtOptions.append(" --amend");

	bool ret = false;

	// get not selected files but updated in index to restore at the end
	const QStringList notSel(getOtherFiles(selFiles, optOnlyInIndex));

	// call git reset to remove not selected files from index
	if (!notSel.empty() && !run("git reset -- " + quote(notSel)))
		goto fail;

	// update index with selected files
	if (!updateIndex(selFiles))
		goto fail;

	// now we can finally commit..
	if (!run("git commit" + cmtOptions + " -F " + quote(msgFile)))
		goto fail;

	// restore not selected files that were already in index
	if (!notSel.empty() && !updateIndex(notSel))
		goto fail;

	ret = true;
fail:
	QDir dir(workDir);
	dir.remove(msgFile);
	return ret;
}

bool Git::mkPatchFromWorkDir(SCRef msg, SCRef patchFile, SCList files) {

 	/* unfortunately 'git diff' sees only files already
	 * known to git or already in index, so update index first
	 * to be sure also unknown files are correctly found
	 */
 	if (!updateIndex(files))
 		return false;

	QString runOutput;
	if (!run("git diff -C HEAD -- " + quote(files), &runOutput))
		return false;

	const QString patch("Subject: " + msg + "\n---\n" + runOutput);
	return writeToFile(patchFile, patch);
}

bool Git::stgCommit(SCList selFiles, SCRef msg, SCRef patchName, bool fold) {

	/* Here the deal is to use 'stg import' and 'stg fold' to add a new
	 * patch or refresh the current one respectively. Unfortunately refresh
	 * does not work with partial selection of files and also does not take
	 * patch message from a file that is needed to avoid artifacts with '\n'
	 * and friends.
	 *
	 * So steps are:
	 *
	 * - Create a patch file with the changes you want to import/fold in StGit
	 * - Stash working dir files because import/fold wants a clean directory
	 * - Import/fold the patch
	 * - Unstash and merge working dir modified files
	 * - Restore index with not selected files
	 */

	/* Step 1: Create a patch file with the changes you want to import/fold */
	bool ret = false;
	const QString patchFile(gitDir + "/qgit_tmp_patch.txt");

	// in case we don't have files to restore we can shortcut various commands
	bool partialSelection = !getOtherFiles(selFiles, !optOnlyInIndex).isEmpty();

	// get not selected files but updated in index to restore at the end
	QStringList notSel;
	if (partialSelection) // otherwise notSel is for sure empty
		notSel = getOtherFiles(selFiles, optOnlyInIndex);

	// create a patch with diffs between working dir and HEAD
	if (!mkPatchFromWorkDir(msg, patchFile, selFiles))
		goto fail;

	/* Step 2: Stash working dir modified files */
	if (partialSelection) {
		errorReportingEnabled = false;
		run("git stash"); // unfortunately 'git stash' is noisy on stderr
		errorReportingEnabled = true;
	}

	/* Step 3: Call stg import/fold */

	// setup a clean state
	if (!run("stg status --reset"))
		goto fail_and_unstash;

	if (fold) {
		// update patch message before to fold, note that
		// command 'stg edit' requires stg version 0.14 or later
		if (!msg.isEmpty() && !run("stg edit --message " + quote(msg.trimmed())))
			goto fail_and_unstash;

		if (!run("stg fold " + quote(patchFile)))
			goto fail_and_unstash;

		if (!run("stg refresh")) // refresh needed after fold
			goto fail_and_unstash;

	} else if (!run("stg import --mail --name " + quote(patchName) + " " + quote(patchFile)))
		goto fail_and_unstash;

	if (partialSelection) {

		/* Step 4: Unstash and merge working dir modified files */
		errorReportingEnabled = false;
		run("git stash pop"); // unfortunately 'git stash' is noisy on stderr
		errorReportingEnabled = true;

		/* Step 5: restore not selected files that were already in index */
		if (!notSel.empty() && !updateIndex(notSel))
			goto fail;
	}

	ret = true;
	goto exit;

fail_and_unstash:

	if (partialSelection) {
		run("git reset");
		errorReportingEnabled = false;
		run("git stash pop");
		errorReportingEnabled = true;
	}
fail:
exit:
	QDir dir(workDir);
	dir.remove(patchFile);
	return ret;
}

bool Git::makeBranch(SCRef sha, SCRef branchName) {

	return run("git branch " + branchName + " " + sha);
}

bool Git::makeTag(SCRef sha, SCRef tagName, SCRef msg) {

	if (msg.isEmpty())
		return run("git tag " + tagName + " " + sha);

	return run("git tag -m \"" + msg + "\" " + tagName + " " + sha);
}

bool Git::deleteTag(SCRef sha) {

	const QStringList tags(getRefName(sha, TAG));
	if (!tags.empty())
		return run("git tag -d " + tags.first()); // only one

	return false;
}

bool Git::stgPush(SCRef sha) {

	const QStringList patch(getRefName(sha, UN_APPLIED));
	if (patch.count() != 1) {
		dbp("ASSERT in Git::stgPush, found %1 patches instead of 1", patch.count());
		return false;
	}
	return run("stg push " + quote(patch.first()));
}

bool Git::stgPop(SCRef sha) {

	const QStringList patch(getRefName(sha, APPLIED));
	if (patch.count() != 1) {
		dbp("ASSERT in Git::stgPop, found %1 patches instead of 1", patch.count());
		return false;
	}
	return run("stg pop " + quote(patch));
}
