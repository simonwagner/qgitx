/*
	Description: qgit main view

	Author: Marco Costalba (C) 2005-2007

	Copyright: See COPYING file that comes with this distribution

*/
#include <QCloseEvent>
#include <QDrag>
#include <QEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QWheelEvent>
#include <QToolButton>
#include <QMimeData>
#include "config.h" // defines PACKAGE_VERSION
#include "commitimpl.h"
#include "common.h"
#include "git.h"
#include "help.h"
#include "historyview.h"
#include "mainimpl.h"
#include "revdesc.h"
#include "revsview.h"
#include "settingsimpl.h"
#include "navigator/navigatorcontroller.h"
#include "filehistory.h"
#include "ui_help.h"
#include "ui_revsview.h"

using namespace QGit;

MainImpl::MainImpl(SCRef cd, QWidget* p) : QMainWindow(p) {

	EM_INIT(exExiting, "Exiting");

	setAttribute(Qt::WA_DeleteOnClose);
	setupUi(this);

    navigatorController = new NavigatorController(*navigator); //TODO: does this magically get deleted by QT?
	// manual setup widgets not buildable with Qt designer
    lineEditFilter = new SearchEdit<ComboSearch>(QPixmap(":/icons/search.svg"), this);
    lineEditFilter->addFilter("Short log", CS_SHORT_LOG);
    lineEditFilter->addFilter("Log msg", CS_LOG_MSG);
    lineEditFilter->addFilter("Author", CS_AUTHOR);
    lineEditFilter->addFilter("SHA1", CS_SHA1);
    lineEditFilter->addFilter("File", CS_FILE);
    lineEditFilter->addFilter("Patch", CS_PATCH);
    lineEditFilter->addFilter("Patch (regExp)", CS_PATCH_REGEXP);
    toolBar->insertWidget(ActSearchAndFilter, lineEditFilter);
	connect(lineEditFilter, SIGNAL(returnPressed()), this, SLOT(lineEditFilter_returnPressed()));

	// create light and dark colors for alternate background
	ODD_LINE_COL = palette().color(QPalette::Base);
	EVEN_LINE_COL = ODD_LINE_COL.dark(103);

	// our interface to git world
	git = new Git(this);
	setupShortcuts();
	qApp->installEventFilter(this);

	// init native types
	setRepositoryBusy = false;

	// init filter match highlighters
	shortLogRE.setMinimal(true);
	shortLogRE.setCaseSensitivity(Qt::CaseInsensitive);
	longLogRE.setMinimal(true);
	longLogRE.setCaseSensitivity(Qt::CaseInsensitive);

	// set-up standard revisions and files list font
	QSettings settings;
	QString font(settings.value(STD_FNT_KEY).toString());
	if (font.isEmpty())
		font = QApplication::font().toString();
	QGit::STD_FONT.fromString(font);

	// set-up typewriter (fixed width) font
	font = settings.value(TYPWRT_FNT_KEY).toString();
	if (font.isEmpty()) { // choose a sensible default
		QFont fnt = QApplication::font();
		fnt.setStyleHint(QFont::TypeWriter, QFont::PreferDefault);
		fnt.setFixedPitch(true);
		fnt.setFamily(fnt.defaultFamily()); // the family corresponding
		font = fnt.toString();              // to current style hint
	}
	QGit::TYPE_WRITER_FONT.fromString(font);

	// set-up tab view
	rv = new RevsView(this, git, true); // set has main domain
    viewStack->addWidget(rv->tabPage());

	// set-up file names loading progress bar
	pbFileNamesLoading = new QProgressBar(statusBar());
	pbFileNamesLoading->setTextVisible(false);
	pbFileNamesLoading->setToolTip("Background file names loading");
	pbFileNamesLoading->hide();
	statusBar()->addPermanentWidget(pbFileNamesLoading);

	QVector<QSplitter*> v(1, treeSplitter);
	QGit::restoreGeometrySetting(QGit::MAIN_GEOM_KEY, this, &v);

	// set-up menu for recent visited repositories
	connect(File, SIGNAL(triggered(QAction*)), this, SLOT(openRecent_triggered(QAction*)));
	doUpdateRecentRepoMenu("");

	// disable all actions
	updateGlobalActions(false);

	connect(git, SIGNAL(fileNamesLoad(int, int)), this, SLOT(fileNamesLoad(int, int)));

	connect(git, SIGNAL(newRevsAdded(const FileHistory*, const QVector<ShaString>&)),
	        this, SLOT(newRevsAdded(const FileHistory*, const QVector<ShaString>&)));

	connect(this, SIGNAL(typeWriterFontChanged()), this, SIGNAL(updateRevDesc()));

	connect(this, SIGNAL(changeFont(const QFont&)), git, SIGNAL(changeFont(const QFont&)));

	// use most recent repo as startup dir if it exists and user opted to do so
	QStringList recents(settings.value(REC_REP_KEY).toStringList());
	QDir checkRepo;
	if (	recents.size() >= 1
		 && testFlag(REOPEN_REPO_F, FLAGS_KEY)
		 && checkRepo.exists(recents.at(0)))
	{
		startUpDir = recents.at(0);
	}
	else {
		startUpDir = (cd.isEmpty() ? QDir::current().absolutePath() : cd);
	}

	// MainImpl c'tor is called before to enter event loop,
	// but some stuff requires event loop to init properly
	QTimer::singleShot(10, this, SLOT(initWithEventLoopActive()));

    //load branches when git is ready
    connect(this->git, &Git::loadCompleted, [this](const FileHistory* fh, const QString& s) {
        Q_UNUSED(fh); Q_UNUSED(s);
        navigatorController->clear();
        for(QString branchName : this->git->getAllRefNames(Git::BRANCH, Git::optOnlyLoaded))
        {
            navigatorController->addBranch(branchName);
        }
        for(QString tagName : this->git->getAllRefNames(Git::TAG, Git::optOnlyLoaded))
        {
            navigatorController->addTag(tagName);
        }
        for(QString remoteName : this->git->getAllRefNames(Git::RMT_BRANCH, Git::optOnlyLoaded))
        {
            navigatorController->addRemote(remoteName);
        }
    });
    //jump to branch ref when selected in navigation
    connect(this->navigatorController, &NavigatorController::branchActivated, [this](const QString& branchName) {
        SCRef refSha(git->getRefSha(branchName));
        rv->st.setSha(refSha);
        UPDATE_DOMAIN(rv);
    });
    connect(this->navigatorController, &NavigatorController::tagActivated, [this](const QString& tagName) {
        SCRef refSha(git->getRefSha(tagName));
        rv->st.setSha(refSha);
        UPDATE_DOMAIN(rv);
    });
    connect(this->navigatorController, &NavigatorController::remoteActivated, [this](const QString& remoteName) {
        SCRef refSha(git->getRefSha(remoteName));
        rv->st.setSha(refSha);
        UPDATE_DOMAIN(rv);
    });
}

void MainImpl::initWithEventLoopActive() {

	git->checkEnvironment();
	setRepository(startUpDir);
	startUpDir = ""; // one shot
}

void MainImpl::saveCurrentGeometry() {

	QVector<QSplitter*> v(1, treeSplitter);
	QGit::saveGeometrySetting(QGit::MAIN_GEOM_KEY, this, &v);
}

void MainImpl::ActBack_activated() {
    //TODO: reimplement functionality that got lost when removing lineEditSHA
}

void MainImpl::ActForward_activated() {
    //TODO: reimplement functionality that got lost when removing lineEditSHA
}

// *************************** ExternalDiffViewer ***************************

void MainImpl::ActExternalDiff_activated() {

	QStringList args;
	QStringList filenames;
	getExternalDiffArgs(&args, &filenames);
	ExternalDiffProc* externalDiff = new ExternalDiffProc(filenames, this);
	externalDiff->setWorkingDirectory(curDir);

	if (!QGit::startProcess(externalDiff, args)) {
		QString text("Cannot start external viewer: ");
		text.append(args[0]);
		QMessageBox::warning(this, "Error - QGit", text);
		delete externalDiff;
	}
}

void MainImpl::getExternalDiffArgs(QStringList* args, QStringList* filenames) {

	// save files to diff in working directory,
	// will be removed by ExternalDiffProc on exit
	QFileInfo f(rv->st.fileName());
	QString prevRevSha(rv->st.diffToSha());
	if (prevRevSha.isEmpty()) { // default to first parent
		const Rev* r = git->revLookup(rv->st.sha());
		prevRevSha = (r && r->parentsCount() > 0 ? r->parent(0) : rv->st.sha());
	}
	QFileInfo fi(f);
	QString fName1(curDir + "/" + rv->st.sha().left(6) + "_" + fi.fileName());
	QString fName2(curDir + "/" + prevRevSha.left(6) + "_" + fi.fileName());

	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

	QByteArray fileContent;
	QString fileSha(git->getFileSha(rv->st.fileName(), rv->st.sha()));
	git->getFile(fileSha, NULL, &fileContent, rv->st.fileName());
	if (!writeToFile(fName1, QString(fileContent)))
		statusBar()->showMessage("Unable to save " + fName1);

	fileSha = git->getFileSha(rv->st.fileName(), prevRevSha);
	git->getFile(fileSha, NULL, &fileContent, rv->st.fileName());
	if (!writeToFile(fName2, QString(fileContent)))
		statusBar()->showMessage("Unable to save " + fName2);

	// get external diff viewer command
	QSettings settings;
	QString extDiff(settings.value(EXT_DIFF_KEY, EXT_DIFF_DEF).toString());

	QApplication::restoreOverrideCursor();

	// if command doesn't have %1 and %2 to denote filenames, add them to end
	if (!extDiff.contains("%1")) {
		extDiff.append(" %1");
	}
	if (!extDiff.contains("%2")) {
		extDiff.append(" %2");
	}

	// set process arguments
	QStringList extDiffArgs = extDiff.split(' ');
	QString curArg;
	for (int i = 0; i < extDiffArgs.count(); i++) {
		curArg = extDiffArgs.value(i);

		// perform any filename replacements that are necessary
		// (done inside the loop to handle whitespace in paths properly)
		curArg.replace("%1", fName2);
		curArg.replace("%2", fName1);

		args->append(curArg);
	}

	// set filenames so that they can be deleted when the process completes
	filenames->append(fName1);
	filenames->append(fName2);
}

// ********************** Repository open or changed *************************

void MainImpl::setRepository(SCRef newDir, bool refresh, bool keepSelection,
                             const QStringList* passedArgs, bool overwriteArgs) {

	/*
	   Because Git::init calls processEvents(), if setRepository() is called in
	   a tight loop (as example keeping pressed F5 refresh button) then a lot
	   of pending init() calls would be stacked.
	   On returning from processEvents() an exception is trown and init is exited,
	   so we end up with a long list of 'exception thrown' messages.
	   But the worst thing is that we have to wait for _all_ the init call to exit
	   and this could take a long time as example in case of working dir refreshing
	   'git update-index' of a big tree.
	   So we use a guard flag to guarantee we have only one init() call 'in flight'
	*/
	if (setRepositoryBusy)
		return;

	setRepositoryBusy = true;

	// check for a refresh or open of a new repository while in filtered view
	try {
		EM_REGISTER(exExiting);

		bool archiveChanged;
		curDir = git->getBaseDir(&archiveChanged, newDir);

		git->stop(archiveChanged); // stop all pending processes, non blocking

		if (archiveChanged && refresh)
			dbs("ASSERT in setRepository: different dir with no range select");

		// now we can clear all our data
		setWindowTitle(curDir + " - QGit");
		bool complete = !refresh || !keepSelection;
		rv->clear(complete);
		if (archiveChanged)
			emit closeAllTabs();

		// disable all actions
		updateGlobalActions(false);
		updateContextActions("", "", false, false);
		ActCommit_setEnabled(false);

        bool ok = git->init(curDir, passedArgs, overwriteArgs); // blocking call

		updateCommitMenu(ok && git->isStGITStack());
		ActCheckWorkDir->setChecked(testFlag(DIFF_INDEX_F)); // could be changed in Git::init()

		if (ok) {
			updateGlobalActions(true);
			if (archiveChanged)
				updateRecentRepoMenu(curDir);
		} else
			statusBar()->showMessage("Not a git archive");

		setRepositoryBusy = false;
		EM_REMOVE(exExiting);

	} catch (int i) {
		EM_REMOVE(exExiting);

		if (EM_MATCH(i, exExiting, "loading repository")) {
			EM_THROW_PENDING;
			return;
		}
		const QString info("Exception \'" + EM_DESC(i) + "\' not "
		                   "handled in setRepository...re-throw");
		dbs(info);
		throw;
	}
}

void MainImpl::updateGlobalActions(bool b) {

	ActRefresh->setEnabled(b);
	ActCheckWorkDir->setEnabled(b);
	ActViewRev->setEnabled(b);

	rv->setEnabled(b);
}

void MainImpl::updateContextActions(SCRef newRevSha, SCRef newFileName,
                                    bool isDir, bool found) {

	bool pathActionsEnabled = !newFileName.isEmpty();
	bool fileActionsEnabled = (pathActionsEnabled && !isDir);

	ActExternalDiff->setEnabled(fileActionsEnabled);
	ActSaveFile->setEnabled(fileActionsEnabled);

	bool isTag, isUnApplied, isApplied;
	isTag = isUnApplied = isApplied = false;

	if (found) {
		const Rev* r = git->revLookup(newRevSha);
		isTag = git->checkRef(newRevSha, Git::TAG);
		isUnApplied = r->isUnApplied;
		isApplied = r->isApplied;
	}
	ActBranch->setEnabled(found && (newRevSha != ZERO_SHA) && !isUnApplied);
	ActTag->setEnabled(found && (newRevSha != ZERO_SHA) && !isUnApplied);
	ActTagDelete->setEnabled(found && isTag && (newRevSha != ZERO_SHA) && !isUnApplied);
	ActPush->setEnabled(found && isUnApplied && git->isNothingToCommit());
	ActPop->setEnabled(found && isApplied && git->isNothingToCommit());
}

// ************************* cross-domain update Actions ***************************

void MainImpl::ActViewRev_activated() {
    viewStack->setCurrentWidget(rv->tabPage());
}

void MainImpl::revisionsDragged(SCList selRevs) {

	const QString h(QString::fromLatin1("@") + curDir + '\n');
	const QString dragRevs = selRevs.join(h).append(h).trimmed();
	QDrag* drag = new QDrag(this);
	QMimeData* mimeData = new QMimeData;
	mimeData->setText(dragRevs);
	drag->setMimeData(mimeData);
	drag->start(); // blocking until drop event
}

void MainImpl::revisionsDropped(SCList remoteRevs) {
// remoteRevs is already sanity checked to contain some possible valid data

	if (rv->isDropping()) // avoid reentrancy
		return;

	QDir dr(curDir + QGit::PATCHES_DIR);
	if (dr.exists()) {
		const QString tmp("Please remove stale import directory " + dr.absolutePath());
		statusBar()->showMessage(tmp);
		return;
	}
	bool workDirOnly, fold;
	if (!askApplyPatchParameters(&workDirOnly, &fold))
		return;

	// ok, let's go
	rv->setDropping(true);
	dr.setFilter(QDir::Files);
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	raise();
	EM_PROCESS_EVENTS;

	uint revNum = 0;
	QStringList::const_iterator it(remoteRevs.constEnd());
	do {
		--it;

		QString tmp("Importing revision %1 of %2");
		statusBar()->showMessage(tmp.arg(++revNum).arg(remoteRevs.count()));

		SCRef sha((*it).section('@', 0, 0));
		SCRef remoteRepo((*it).section('@', 1));

		if (!dr.exists(remoteRepo))
			break;

		// we create patches one by one
		if (!git->formatPatch(QStringList(sha), dr.absolutePath(), remoteRepo))
			break;

		dr.refresh();
		if (dr.count() != 1) {
			qDebug("ASSERT in on_droppedRevisions: found %i files "
			       "in %s", dr.count(), QGit::PATCHES_DIR.toLatin1().constData());
			break;
		}
		SCRef fn(dr.absoluteFilePath(dr[0]));
		bool is_applied = git->applyPatchFile(fn, fold, Git::optDragDrop);
		dr.remove(fn);
		if (!is_applied)
			break;

	} while (it != remoteRevs.constBegin());

	if (it == remoteRevs.constBegin())
		statusBar()->clearMessage();
	else
		statusBar()->showMessage("Failed to import revision " + QString::number(revNum--));

	if (workDirOnly && (revNum > 0))
		git->resetCommits(revNum);

	dr.rmdir(dr.absolutePath()); // 'dr' must be already empty
	QApplication::restoreOverrideCursor();
	rv->setDropping(false);
	refreshRepo();
}

// ******************************* Filter ******************************

void MainImpl::newRevsAdded(const FileHistory* fh, const QVector<ShaString>&) {

	if (!git->isMainHistory(fh))
		return;

	if (ActSearchAndFilter->isChecked())
		ActSearchAndFilter_toggled(true); // filter again on new arrived data

	if (ActSearchAndHighlight->isChecked())
		ActSearchAndHighlight_toggled(true); // filter again on new arrived data

	// first rev could be a StGIT unapplied patch so check more then once
	if (   !ActCommit->isEnabled()
	    && (!git->isNothingToCommit() || git->isUnknownFiles())
	    && !git->isCommittingMerge())
		ActCommit_setEnabled(true);
}

void MainImpl::lineEditFilter_returnPressed() {

	ActSearchAndFilter->setChecked(true);
}

void MainImpl::ActSearchAndFilter_toggled(bool isOn) {

	ActSearchAndHighlight->setEnabled(!isOn);
	ActSearchAndFilter->setEnabled(false);
	filterList(isOn, false); // blocking call
	ActSearchAndFilter->setEnabled(true);
}

void MainImpl::ActSearchAndHighlight_toggled(bool isOn) {

	ActSearchAndFilter->setEnabled(!isOn);
	ActSearchAndHighlight->setEnabled(false);
	filterList(isOn, true); // blocking call
	ActSearchAndHighlight->setEnabled(true);
}

void MainImpl::filterList(bool isOn, bool onlyHighlight) {

	lineEditFilter->setEnabled(!isOn);

	SCRef filter(lineEditFilter->text());
	if (filter.isEmpty())
		return;

	ShaSet shaSet;
	bool patchNeedsUpdate, isRegExp;
	patchNeedsUpdate = isRegExp = false;
    int idx = lineEditFilter->selectedFilter(), colNum = 0;
	if (isOn) {
		switch (idx) {
		case CS_SHORT_LOG:
			colNum = LOG_COL;
			shortLogRE.setPattern(filter);
			break;
		case CS_LOG_MSG:
			colNum = LOG_MSG_COL;
			longLogRE.setPattern(filter);
			break;
		case CS_AUTHOR:
			colNum = AUTH_COL;
			break;
		case CS_SHA1:
			colNum = COMMIT_COL;
			break;
		case CS_FILE:
		case CS_PATCH:
		case CS_PATCH_REGEXP:
			colNum = SHA_MAP_COL;
			QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
			EM_PROCESS_EVENTS; // to paint wait cursor
			if (idx == CS_FILE)
				git->getFileFilter(filter, shaSet);
			else {
				isRegExp = (idx == CS_PATCH_REGEXP);
				if (!git->getPatchFilter(filter, isRegExp, shaSet)) {
					QApplication::restoreOverrideCursor();
					ActSearchAndFilter->toggle();
					return;
				}
				patchNeedsUpdate = (shaSet.count() > 0);
			}
			QApplication::restoreOverrideCursor();
			break;
		}
	} else {
		patchNeedsUpdate = (idx == CS_PATCH || idx == CS_PATCH_REGEXP);
		shortLogRE.setPattern("");
		longLogRE.setPattern("");
	}
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    HistoryView* lv = rv->tab()->listViewLog;
	int matchedCnt = lv->filterRows(isOn, onlyHighlight, filter, colNum, &shaSet);

	QApplication::restoreOverrideCursor();

	emit updateRevDesc(); // could be highlighted
	if (patchNeedsUpdate)
		emit highlightPatch(isOn ? filter : "", isRegExp);

	QString msg;
	if (isOn && !onlyHighlight)
		msg = QString("Found %1 matches. Toggle filter/highlight "
		              "button to remove the filter").arg(matchedCnt);
	QApplication::postEvent(rv, new MessageEvent(msg)); // deferred message, after update
}

bool MainImpl::event(QEvent* e) {

	BaseEvent* de = dynamic_cast<BaseEvent*>(e);
	if (!de)
		return QWidget::event(e);

	SCRef data = de->myData();
	bool ret = true;

    switch ((EventType)e->type()) {
	case ERROR_EV: {
		QApplication::setOverrideCursor(QCursor(Qt::ArrowCursor));
		EM_PROCESS_EVENTS;
		MainExecErrorEvent* me = (MainExecErrorEvent*)e;
		QString text("An error occurred while executing command:\n\n");
		text.append(me->command() + "\n\nGit says: \n\n" + me->report());
		QMessageBox::warning(this, "Error - QGit", text);
		QApplication::restoreOverrideCursor(); }
		break;
	case MSG_EV:
		statusBar()->showMessage(data);
		break;
	case POPUP_LIST_EV:
		doContexPopup(data);
		break;
	case POPUP_FILE_EV:
	case POPUP_TREE_EV:
		doFileContexPopup(data, e->type());
		break;
	default:
		dbp("ASSERT in MainImpl::event unhandled event %1", e->type());
		ret = false;
		break;
	}
	return ret;
}

int MainImpl::currentTabType(Domain** t) {
    //FIXME: adapt this to new code, when ready
    Q_UNUSED(t);
    return TAB_REV;
}

void MainImpl::setupShortcuts() {

	new QShortcut(Qt::Key_I,     this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_K,     this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_N,     this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_Left,  this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_Right, this, SLOT(shortCutActivated()));

	new QShortcut(Qt::Key_Delete,    this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_Backspace, this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_Space,     this, SLOT(shortCutActivated()));

	new QShortcut(Qt::Key_B, this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_D, this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_F, this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_P, this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_R, this, SLOT(shortCutActivated()));
	new QShortcut(Qt::Key_U, this, SLOT(shortCutActivated()));

	new QShortcut(Qt::SHIFT | Qt::Key_Up,    this, SLOT(shortCutActivated()));
	new QShortcut(Qt::SHIFT | Qt::Key_Down,  this, SLOT(shortCutActivated()));
	new QShortcut(Qt::CTRL  | Qt::Key_Plus,  this, SLOT(shortCutActivated()));
	new QShortcut(Qt::CTRL  | Qt::Key_Minus, this, SLOT(shortCutActivated()));
}

void MainImpl::shortCutActivated() {

	QShortcut* se = dynamic_cast<QShortcut*>(sender());
	if (!se)
		return;
    switch (se->key()[0]) {

	case Qt::Key_I:
		rv->tab()->listViewLog->on_keyUp();
		break;
	case Qt::Key_K:
	case Qt::Key_N:
		rv->tab()->listViewLog->on_keyDown();
		break;
	case Qt::SHIFT | Qt::Key_Up:
		goMatch(-1);
		break;
	case Qt::SHIFT | Qt::Key_Down:
		goMatch(1);
		break;
	case Qt::Key_Left:
		ActBack_activated();
		break;
	case Qt::Key_Right:
		ActForward_activated();
		break;
	case Qt::CTRL | Qt::Key_Plus:
		adjustFontSize(1);
		break;
	case Qt::CTRL | Qt::Key_Minus:
		adjustFontSize(-1);
		break;
	case Qt::Key_U:
		scrollTextEdit(-18);
		break;
	case Qt::Key_D:
		scrollTextEdit(18);
		break;
	case Qt::Key_Delete:
	case Qt::Key_B:
	case Qt::Key_Backspace:
		scrollTextEdit(-1);
		break;
	case Qt::Key_Space:
		scrollTextEdit(1);
		break;
	case Qt::Key_R:
        viewStack->setCurrentWidget(rv->tabPage());
		break;
	}
}

void MainImpl::goMatch(int delta) {

	if (ActSearchAndHighlight->isChecked())
		rv->tab()->listViewLog->scrollToNextHighlighted(delta);
}

QTextEdit* MainImpl::getCurrentTextEdit() {

	QTextEdit* te = NULL;
    //FIXME RevsView is no longer a QTextEdit widget
    /*Domain* t;
	switch (currentTabType(&t)) {
	case TAB_REV:
		te = static_cast<RevsView*>(t)->tab()->textBrowserDesc;
	default:
		break;
    }*/
	return te;
}

void MainImpl::scrollTextEdit(int delta) {

	QTextEdit* te = getCurrentTextEdit();
	if (!te)
		return;

	QScrollBar* vs = te->verticalScrollBar();
	if (delta == 1 || delta == -1)
		vs->setValue(vs->value() + delta * (vs->pageStep() - vs->singleStep()));
	else
		vs->setValue(vs->value() + delta * vs->singleStep());
}

void MainImpl::adjustFontSize(int delta) {
// font size is changed on a 'per instance' base and only on list views

	int ps = QGit::STD_FONT.pointSize() + delta;
	if (ps < 2)
		return;

	QGit::STD_FONT.setPointSize(ps);

	QSettings settings;
	settings.setValue(QGit::STD_FNT_KEY, QGit::STD_FONT.toString());
	emit changeFont(QGit::STD_FONT);
}

void MainImpl::fileNamesLoad(int status, int value) {

	switch (status) {
	case 1: // stop
		pbFileNamesLoading->hide();
		break;
	case 2: // update
		pbFileNamesLoading->setValue(value);
		break;
	case 3: // start
		if (value > 200) { // don't show for few revisions
			pbFileNamesLoading->reset();
			pbFileNamesLoading->setMaximum(value);
			pbFileNamesLoading->show();
		}
		break;
	}
}

// ****************************** Menu *********************************

void MainImpl::updateCommitMenu(bool isStGITStack) {

	ActCommit->setText(isStGITStack ? "Commit St&GIT patch..." : "&Commit...");
	ActAmend->setText(isStGITStack ? "Refresh St&GIT patch..." : "&Amend commit...");
}

void MainImpl::updateRecentRepoMenu(SCRef newEntry) {

	// update menu of all windows
	foreach (QWidget* widget, QApplication::topLevelWidgets()) {

		MainImpl* w = dynamic_cast<MainImpl*>(widget);
		if (w)
			w->doUpdateRecentRepoMenu(newEntry);
	}
}

void MainImpl::doUpdateRecentRepoMenu(SCRef newEntry) {

	QList<QAction*> al(File->actions());
	FOREACH (QList<QAction*>, it, al) {
		SCRef txt = (*it)->text();
		if (!txt.isEmpty() && txt.at(0).isDigit())
			File->removeAction(*it);
	}
	QSettings settings;
	QStringList recents(settings.value(REC_REP_KEY).toStringList());
	int idx = recents.indexOf(newEntry);
	if (idx != -1)
		recents.removeAt(idx);

	if (!newEntry.isEmpty())
		recents.prepend(newEntry);

	idx = 1;
	QStringList newRecents;
	FOREACH_SL (it, recents) {
		File->addAction(QString::number(idx++) + " " + *it);
		newRecents << *it;
		if (idx > MAX_RECENT_REPOS)
			break;
	}
	settings.setValue(REC_REP_KEY, newRecents);
}

static int cntMenuEntries(const QMenu& menu) {

	int cnt = 0;
	QList<QAction*> al(menu.actions());
	FOREACH (QList<QAction*>, it, al) {
		if (!(*it)->isSeparator())
			cnt++;
	}
	return cnt;
}

void MainImpl::doContexPopup(SCRef sha) {

	QMenu contextMenu(this);
	QMenu contextBrnMenu("More branches...", this);
	QMenu contextTagMenu("More tags...", this);
	QMenu contextRmtMenu("Remote branches", this);

	connect(&contextMenu, SIGNAL(triggered(QAction*)), this, SLOT(goRef_triggered(QAction*)));

	Domain* t;
	int tt = currentTabType(&t);
	bool isRevPage = (tt == TAB_REV);

    if (ActCheckWorkDir->isEnabled()) {
		contextMenu.addAction(ActCheckWorkDir);
		contextMenu.addSeparator();
	}

    if (ActExternalDiff->isEnabled())
		contextMenu.addAction(ActExternalDiff);

	if (isRevPage) {
		if (ActCommit->isEnabled() && (sha == ZERO_SHA))
			contextMenu.addAction(ActCommit);
		if (ActBranch->isEnabled())
			contextMenu.addAction(ActBranch);
		if (ActTag->isEnabled())
			contextMenu.addAction(ActTag);
		if (ActTagDelete->isEnabled())
			contextMenu.addAction(ActTagDelete);
		if (ActPush->isEnabled())
			contextMenu.addAction(ActPush);
		if (ActPop->isEnabled())
			contextMenu.addAction(ActPop);

		const QStringList& bn(git->getAllRefNames(Git::BRANCH, Git::optOnlyLoaded));
		const QStringList& rbn(git->getAllRefNames(Git::RMT_BRANCH, Git::optOnlyLoaded));
		const QStringList& tn(git->getAllRefNames(Git::TAG, Git::optOnlyLoaded));
		QAction* act = NULL;

		FOREACH_SL (it, rbn) {
			act = contextRmtMenu.addAction(*it);
			act->setData("Ref");
		}
		if (!contextRmtMenu.isEmpty())
			contextMenu.addMenu(&contextRmtMenu);

		// halve the possible remaining entries for branches and tags
		int remainingEntries = (MAX_MENU_ENTRIES - cntMenuEntries(contextMenu));
		int tagEntries = remainingEntries / 2;
		int brnEntries = remainingEntries - tagEntries;

		// display more branches, if there are few tags
		if (tagEntries > tn.count())
			tagEntries = tn.count();

		// one branch less because of the "More branches..." submenu
		if ((bn.count() > brnEntries) && tagEntries)
			tagEntries++;

		if (!bn.empty())
			contextMenu.addSeparator();

		FOREACH_SL (it, bn) {
			if (   cntMenuEntries(contextMenu) < MAX_MENU_ENTRIES - tagEntries
			    || (*it == bn.last() && contextBrnMenu.isEmpty()))
				act = contextMenu.addAction(*it);
			else
				act = contextBrnMenu.addAction(*it);

			act->setData("Ref");
		}
		if (!contextBrnMenu.isEmpty())
			contextMenu.addMenu(&contextBrnMenu);

		if (!tn.empty())
			contextMenu.addSeparator();

		FOREACH_SL (it, tn) {
			if (   cntMenuEntries(contextMenu) < MAX_MENU_ENTRIES
			    || (*it == tn.last() && contextTagMenu.isEmpty()))
				act = contextMenu.addAction(*it);
			else
				act = contextTagMenu.addAction(*it);

			act->setData("Ref");
		}
		if (!contextTagMenu.isEmpty())
			contextMenu.addMenu(&contextTagMenu);
	}
	contextMenu.exec(QCursor::pos());
}

void MainImpl::doFileContexPopup(SCRef fileName, int type) {

	QMenu contextMenu(this);

	Domain* t;
	int tt = currentTabType(&t);
	bool isRevPage = (tt == TAB_REV);
    bool isDir = QFileInfo(fileName).isDir();

	if (!isRevPage && (type == POPUP_FILE_EV) && ActViewRev->isEnabled())
		contextMenu.addAction(ActViewRev);

	if (!isDir) {
		if (ActSaveFile->isEnabled())
			contextMenu.addAction(ActSaveFile);
		if ((type == POPUP_FILE_EV) && ActExternalDiff->isEnabled())
			contextMenu.addAction(ActExternalDiff);
	}
	contextMenu.exec(QCursor::pos());
}

void MainImpl::goRef_triggered(QAction* act) {

	if (!act || act->data() != "Ref")
		return;

	SCRef refSha(git->getRefSha(act->text()));
	rv->st.setSha(refSha);
	UPDATE_DOMAIN(rv);
}

const QString MainImpl::getRevisionDesc(SCRef sha) {
    return git->getDesc(sha, NULL);
}

void MainImpl::ActSaveFile_activated() {

	QFileInfo f(rv->st.fileName());
	const QString fileName(QFileDialog::getSaveFileName(this, "Save file as", f.fileName()));
	if (fileName.isEmpty())
		return;

	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	QString fileSha(git->getFileSha(rv->st.fileName(), rv->st.sha()));
	if (!git->saveFile(fileSha, rv->st.fileName(), fileName))
		statusBar()->showMessage("Unable to save " + fileName);

	QApplication::restoreOverrideCursor();
}

void MainImpl::openRecent_triggered(QAction* act) {

	bool ok;
	act->text().left(1).toInt(&ok);
	if (!ok) // only recent repos entries have a number in first char
		return;

	const QString workDir(act->text().section(' ', 1));
	if (!workDir.isEmpty()) {
		QDir d(workDir);
		if (d.exists())
			setRepository(workDir);
		else
			statusBar()->showMessage("Directory '" + workDir +
			                         "' does not seem to exsist anymore");
	}
}

void MainImpl::ActOpenRepo_activated() {

	const QString dirName(QFileDialog::getExistingDirectory(this, "Choose a directory", curDir));
	if (!dirName.isEmpty()) {
		QDir d(dirName);
		setRepository(d.absolutePath());
	}
}

void MainImpl::ActOpenRepoNewWindow_activated() {

	const QString dirName(QFileDialog::getExistingDirectory(this, "Choose a directory", curDir));
	if (!dirName.isEmpty()) {
		QDir d(dirName);
		MainImpl* newWin = new MainImpl(d.absolutePath());
		newWin->show();
	}
}

void MainImpl::refreshRepo(bool b) {

	setRepository(curDir, true, b);
}

void MainImpl::ActRefresh_activated() {

	refreshRepo(true);
}

bool MainImpl::askApplyPatchParameters(bool* workDirOnly, bool* fold) {

	int ret = 0;
	if (!git->isStGITStack()) {
		ret = QMessageBox::question(this, "Apply Patch",
		      "Do you want to commit or just to apply changes to "
		      "working directory?", "&Cancel", "&Working dir", "&Commit", 0, 0);
		*workDirOnly = (ret == 1);
		*fold = false;
	} else {
		ret = QMessageBox::question(this, "Apply Patch", "Do you want to "
		      "import or fold the patch?", "&Cancel", "&Fold", "&Import", 0, 0);
		*workDirOnly = false;
		*fold = (ret == 1);
	}
	return (ret != 0);
}

void MainImpl::ActCheckWorkDir_toggled(bool b) {

	if (!ActCheckWorkDir->isEnabled()) // to avoid looping with setChecked()
		return;

	setFlag(DIFF_INDEX_F, b);
	bool keepSelection = (rv->st.sha() != ZERO_SHA);
	refreshRepo(keepSelection);
}

void MainImpl::ActSettings_activated() {

	SettingsImpl setView(this, git);
	connect(&setView, SIGNAL(typeWriterFontChanged()),
	        this, SIGNAL(typeWriterFontChanged()));

	connect(&setView, SIGNAL(flagChanged(uint)),
	        this, SIGNAL(flagChanged(uint)));

	setView.exec();

	// update ActCheckWorkDir if necessary
	if (ActCheckWorkDir->isChecked() != testFlag(DIFF_INDEX_F))
		ActCheckWorkDir->toggle();
}

void MainImpl::ActCommit_activated() {

	CommitImpl* c = new CommitImpl(git, false); // has Qt::WA_DeleteOnClose attribute
	connect(this, SIGNAL(closeAllWindows()), c, SLOT(close()));
	connect(c, SIGNAL(changesCommitted(bool)), this, SLOT(changesCommitted(bool)));
	c->show();
}

void MainImpl::ActAmend_activated() {

	CommitImpl* c = new CommitImpl(git, true); // has Qt::WA_DeleteOnClose attribute
	connect(this, SIGNAL(closeAllWindows()), c, SLOT(close()));
	connect(c, SIGNAL(changesCommitted(bool)), this, SLOT(changesCommitted(bool)));
	c->show();
}

void MainImpl::changesCommitted(bool ok) {

	if (ok)
		refreshRepo(false);
	else
		statusBar()->showMessage("Failed to commit changes");
}

void MainImpl::ActCommit_setEnabled(bool b) {

	// pop and push commands fail if there are local changes,
	// so in this case we disable ActPop and ActPush
	if (b) {
		ActPush->setEnabled(false);
		ActPop->setEnabled(false);
	}
	ActCommit->setEnabled(b);
}

void MainImpl::ActBranch_activated() {

    doBranchOrTag(false);
}

void MainImpl::ActTag_activated() {

    doBranchOrTag(true);
}

void MainImpl::doBranchOrTag(bool isTag) {

	QString refDesc = isTag ? "tag" : "branch";
	QString boxDesc = "Make " + refDesc + " - QGit";
	QString ref(rv->tab()->listViewLog->currentText(LOG_COL));
	bool ok;
	ref = QInputDialog::getText(this, boxDesc, "Enter " + refDesc
	                            + " name:", QLineEdit::Normal, ref, &ok);
	if (!ok || ref.isEmpty())
		return;

	QString tmp(ref.trimmed());
	if (ref != tmp.remove(' ')) {
		QMessageBox::warning(this, boxDesc,
		             "Sorry, control characters or spaces\n"
		             "are not allowed in " + refDesc + " name.");
		return;
	}
	if (!git->getRefSha(ref, isTag ? Git::TAG : Git::BRANCH, false).isEmpty()) {
		QMessageBox::warning(this, boxDesc,
		             "Sorry, " + refDesc + " name already exists.\n"
		             "Please choose a different name.");
		return;
	}
	QString msg;
	if (isTag)
	    msg = QInputDialog::getText(this, boxDesc, "Enter tag message, if any:",
	                                QLineEdit::Normal, "", &ok);

	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	if (isTag)
        ok = git->makeTag(rv->st.sha(), ref, msg);
	else
        ok = git->makeBranch(rv->st.sha(), ref);

	QApplication::restoreOverrideCursor();
	if (ok)
		refreshRepo(true);
	else
		statusBar()->showMessage("Sorry, unable to tag the revision");
}

void MainImpl::ActTagDelete_activated() {

	if (QMessageBox::question(this, "Delete tag - QGit",
	                 "Do you want to un-tag selected revision?",
	                 "&Yes", "&No", QString(), 0, 1) == 1)
		return;

	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    bool ok = git->deleteTag(rv->st.sha());
	QApplication::restoreOverrideCursor();
	if (ok)
		refreshRepo(true);
	else
		statusBar()->showMessage("Sorry, unable to un-tag the revision");
}

void MainImpl::ActPush_activated() {

	QStringList selectedItems;
	rv->tab()->listViewLog->getSelectedItems(selectedItems);
	for (int i = 0; i < selectedItems.count(); i++) {
		if (!git->checkRef(selectedItems[i], Git::UN_APPLIED)) {
			statusBar()->showMessage("Please, select only unapplied patches");
			return;
		}
	}
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	bool ok = true;
	for (int i = 0; i < selectedItems.count(); i++) {
		const QString tmp(QString("Pushing patch %1 of %2")
		                  .arg(i+1).arg(selectedItems.count()));
		statusBar()->showMessage(tmp);
		SCRef sha = selectedItems[selectedItems.count() - i - 1];
		if (!git->stgPush(sha)) {
			statusBar()->showMessage("Failed to push patch " + sha);
			ok = false;
			break;
		}
	}
	if (ok)
		statusBar()->clearMessage();

	QApplication::restoreOverrideCursor();
	refreshRepo(false);
}

void MainImpl::ActPop_activated() {

	QStringList selectedItems;
	rv->tab()->listViewLog->getSelectedItems(selectedItems);
	if (selectedItems.count() > 1) {
		statusBar()->showMessage("Please, select one revision only");
		return;
	}
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	git->stgPop(selectedItems[0]);
	QApplication::restoreOverrideCursor();
	refreshRepo(false);
}

void MainImpl::ActFindNext_activated() {

	QTextEdit* te = getCurrentTextEdit();
	if (!te || textToFind.isEmpty())
		return;

	bool endOfDocument = false;
	while (true) {
		if (te->find(textToFind))
			return;

		if (endOfDocument) {
			QMessageBox::warning(this, "Find text - QGit", "Text \"" +
			             textToFind + "\" not found!", QMessageBox::Ok, 0);
			return;
		}
		if (QMessageBox::question(this, "Find text - QGit", "End of document "
		    "reached\n\nDo you want to continue from beginning?", QMessageBox::Yes,
		    QMessageBox::No | QMessageBox::Escape) == QMessageBox::No)
			return;

		endOfDocument = true;
		te->moveCursor(QTextCursor::Start);
	}
}

void MainImpl::ActFind_activated() {

	QTextEdit* te = getCurrentTextEdit();
	if (!te)
		return;

	QString def(textToFind);
	if (te->textCursor().hasSelection())
		def = te->textCursor().selectedText().section('\n', 0, 0);
	else
		te->moveCursor(QTextCursor::Start);

	bool ok;
	QString str(QInputDialog::getText(this, "Find text - QGit", "Text to find:",
	                                  QLineEdit::Normal, def, &ok));
	if (!ok || str.isEmpty())
		return;

	textToFind = str; // update with valid data only
	ActFindNext_activated();
}

void MainImpl::ActHelp_activated() {

	QDialog* dlg = new QDialog();
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	Ui::HelpBase ui;
	ui.setupUi(dlg);
	ui.textEditHelp->setHtml(QString::fromLatin1(helpInfo)); // defined in help.h
	connect(this, SIGNAL(closeAllWindows()), dlg, SLOT(close()));
	dlg->show();
	dlg->raise();
}

void MainImpl::ActAbout_activated() {

	static const char* aboutMsg =
	"<p><b>QGit version " PACKAGE_VERSION "</b></p>"
	"<p>Copyright (c) 2005, 2007, 2008 Marco Costalba</p>"
	"<p>Use and redistribute under the terms of the<br>"
	"<a href=\"http://www.gnu.org/licenses/old-licenses/gpl-2.0.html\">GNU General Public License Version 2</a></p>"
	"<p>Contributors:<br>"
	"Copyright (c) 2007 Andy Parkins<br>"
	"Copyright (c) 2007 Pavel Roskin<br>"
	"Copyright (c) 2007 Peter Oberndorfer<br>"
	"Copyright (c) 2007 Yaacov Akiba<br>"
	"Copyright (c) 2007 James McKaskill<br>"
	"Copyright (c) 2008 Jan Hudec<br>"
	"Copyright (c) 2008 Paul Gideon Dann<br>"
	"Copyright (c) 2008 Oliver Bock</p>"
	"<p>This version was compiled against Qt " QT_VERSION_STR "</p>";
	QMessageBox::about(this, "About QGit", QString::fromLatin1(aboutMsg));
}

void MainImpl::closeEvent(QCloseEvent* ce) {

	saveCurrentGeometry();

	// lastWindowClosed() signal is emitted by close(), after sending
	// closeEvent(), so we need to close _here_ all secondary windows before
	// the close() method checks for lastWindowClosed flag to avoid missing
	// the signal and stay in the main loop forever, because lastWindowClosed()
	// signal is connected to qApp->quit()
	//
	// note that we cannot rely on setting 'this' parent in secondary windows
	// because when close() is called children are still alive and, finally,
	// when children are deleted, d'tor do not call close() anymore. So we miss
	// lastWindowClosed() signal in this case.
	emit closeAllWindows();
	hide();

	EM_RAISE(exExiting);

	git->stop(Git::optSaveCache);

	if (!git->findChildren<QProcess*>().isEmpty()) {
		// if not all processes have been deleted, there is
		// still some run() call not returned somewhere, it is
		// not safe to delete run() callers objects now
		QTimer::singleShot(100, this, SLOT(ActClose_activated()));
		ce->ignore();
		return;
	}
	emit closeAllTabs();
	delete rv;
	QWidget::closeEvent(ce);
}

void MainImpl::ActClose_activated() {

	close();
}

void MainImpl::ActExit_activated() {

	qApp->closeAllWindows();
}
