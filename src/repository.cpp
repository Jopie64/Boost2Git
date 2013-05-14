/*
 *  Copyright (C) 2007  Thiago Macieira <thiago@kde.org>
 *  Copyright (C) 2009 Thomas Zander <zander@kde.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "repository.h"
#include "options.hpp"
#include "log.hpp"
#include <boost/range/algorithm/count.hpp>
#include <QTextStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLinkedList>
#include <boost/foreach.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <iomanip>

static const int maxSimultaneousProcesses = 100;

static const int maxMark = (1 << 20) - 2; // some versions of git-fast-import are buggy for larger values of maxMark

class ProcessCache: QLinkedList<Repository *>
  {
  public:
    void touch(Repository *repo)
      {
      remove(repo);

      // if the cache is too big, remove from the front
      while (size() >= maxSimultaneousProcesses)
          takeFirst()->closeFastImport();

      // append to the end
      append(repo);
      }

    inline void remove(Repository *repo)
      {
#if QT_VERSION >= 0x040400
      removeOne(repo);
#else
      removeAll(repo);
#endif
      }
  };
static ProcessCache processCache;

static QString marksFileName(std::string name_)
  {
  QString name = QString::fromStdString(name_);
  name.replace('/', '_');
  name.prepend("marks-");
  return name;
  }

Repository::Repository(
    const Ruleset::Repository &rule,
    bool incremental,
    QHash<QString, Repository*> const& repo_index)
    : name(rule.name)
    , prefix(/*rule.forwardTo*/)
    , submodule_in_repo(
        rule.submodule_in_repo.empty()
        ? 0 : repo_index[QString::fromStdString(rule.submodule_in_repo)] )
    , submodule_path( rule.submodule_path )
    , fastImport(name)
    , commitCount(0)
    , last_commit_mark(0)
    , next_file_mark(maxMark)
    , processHasStarted(false)
    , incremental(incremental)
  {
  BOOST_FOREACH(boost2git::BranchRule const* branch, rule.branches)
    {
    branches[git_ref_name(branch)].lastChangeRev = Branch::neverChanged;
    }

  // create the default branch
  branches["refs/heads/master"].lastChangeRev = 1;

  QString qname = QString::fromStdString(name);
  fastImport.setWorkingDirectory(qname);
  if (!options.dry_run) {
    if (!QDir(qname).exists()) { // repo doesn't exist yet.
      Log::trace() << "Creating new repository " << name << std::endl;
      QDir::current().mkpath(qname);
      QProcess init;
      init.setWorkingDirectory(qname);
      init.start("git", QStringList() << "--bare" << "init");
      init.waitForFinished(-1);
//            // Write description
//            if (!rule.description.empty()) {
//                QFile fDesc(QDir(name).filePath("description"));
//                if (fDesc.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
//                    fDesc.write(rule.description);
//                    fDesc.putChar('\n');
//                    fDesc.close();
//                }
//            }
      {
      QFile marks(qname + "/" + marksFileName(name));
      marks.open(QIODevice::WriteOnly);
      marks.close();
      }
    }
  }
  }

static QString logFileName(std::string name_)
  {
  QString name = QString::fromStdString(name_);
  name.replace('/', '_');
  name.prepend("log-");
  return name;
  }

static int lastValidMark(std::string name)
  {
  QString qname = QString::fromStdString(name);
  QFile marksfile(qname + "/" + marksFileName(name));
  if (!marksfile.open(QIODevice::ReadOnly))
      return 0;

  int prev_mark = 0;

  int lineno = 0;
  while (!marksfile.atEnd()) {
    QByteArray line = marksfile.readLine();
    ++lineno;
    if (line.isEmpty())
        continue;

    int mark = 0;
    if (line[0] == ':') {
      int sp = line.indexOf(' ');
      if (sp != -1) {
        QByteArray m = line.mid(1, sp-1);
        mark = m.toInt();
      }
    }

    if (!mark) {
      qCritical() << marksfile.fileName() << "line" << lineno << "marks file corrupt?";
      return 0;
    }

    if (mark == prev_mark) {
      qCritical() << marksfile.fileName() << "line" << lineno << "marks file has duplicates";
      return 0;
    }

    if (mark < prev_mark) {
      qCritical() << marksfile.fileName() << "line" << lineno << "marks file not sorted";
      return 0;
    }

    if (mark > prev_mark + 1)
        break;

    prev_mark = mark;
  }

  return prev_mark;
  }

int Repository::setupIncremental(int &cutoff)
  {
  QFile logfile(logFileName(name));
  if (!logfile.exists())
      return 1;

  logfile.open(QIODevice::ReadWrite);

  QRegExp progress("progress SVN r(\\d+) branch (.*) = :(\\d+)");

  int last_valid_mark = lastValidMark(name);

  int last_revnum = 0;
  qint64 pos = 0;
  int retval = 0;
  QString bkup = logfile.fileName() + ".old";

  while (!logfile.atEnd()) {
    pos = logfile.pos();
    QByteArray line = logfile.readLine();
    int hash = line.indexOf('#');
    if (hash != -1)
        line.truncate(hash);
    line = line.trimmed();
    if (line.isEmpty())
        continue;
    if (!progress.exactMatch(line))
        continue;

    int revnum = progress.cap(1).toInt();
    QString qbranch = progress.cap(2);
    int mark = progress.cap(3).toInt();

    if (revnum >= cutoff)
        goto beyond_cutoff;

    if (revnum < last_revnum)
      {
      Log::warn() << name << " revision numbers are not monotonic: "
                  << " got " << last_revnum << " and then " << revnum << std::endl;
      }

    if (mark > last_valid_mark)
      {
      Log::warn() << name << " unknown commit mark found:"
                  << " rewinding -- did you hit Ctrl-C?" << std::endl;
      cutoff = revnum;
      goto beyond_cutoff;
      }

    last_revnum = revnum;

    if (last_commit_mark < mark)
        last_commit_mark = mark;

    Branch &br = branches[qbranch.toStdString()];
    if (!br.exists() || !mark)
      {
      br.lastChangeRev = revnum;
      }
    br.commits.append(revnum);
    br.marks.append(mark);
  }

  retval = last_revnum + 1;
  if (retval == cutoff)
      /*
       * If a stale backup file exists already, remove it, so that
       * we don't confuse ourselves in 'restoreLog()'
       */
      QFile::remove(bkup);

  return retval;

beyond_cutoff:
  // backup file, since we'll truncate
  QFile::remove(bkup);
  logfile.copy(bkup);

  // truncate, so that we ignore the rest of the revisions
  Log::debug() << name << " truncating history to revision "
               << cutoff << std::endl;
  logfile.resize(pos);
  return cutoff;
  }

void Repository::restoreLog()
  {
  QString file = logFileName(name);
  QString bkup = file + ".old";
  if (!QFile::exists(bkup))
      return;
  QFile::remove(file);
  QFile::rename(bkup, file);
  }

Repository::~Repository()
  {
  Q_ASSERT(transactions.size() == 0);
  closeFastImport();
  }

void Repository::closeFastImport()
  {
  if (fastImport.state() != QProcess::NotRunning)
    {
    fastImport.write("checkpoint\n");
    fastImport.waitForBytesWritten(-1);
    fastImport.closeWriteChannel();
    if (!fastImport.waitForFinished())
      {
      fastImport.terminate();
      if (!fastImport.waitForFinished(200))
        {
        Log::warn() << "git-fast-import for repository "
                    << name << " did not die" << std::endl;
        }
      }
    }
  processHasStarted = false;
  processCache.remove(this);
  }

void Repository::reloadBranches()
  {
  bool reset_notes = false;
  foreach (std::string branch, branches.keys()) {
    Q_ASSERT(boost::starts_with(branch, "refs/"));
    Branch &br = branches[branch];

    if (br.marks.isEmpty() || !br.marks.last())
        continue;

    reset_notes = true;

    std::string branchRef = branch;

    fastImport.write("reset " + branchRef +
      "\nfrom :" + to_string(br.marks.last()) + "\n\n"
      "progress Branch " + branchRef + " reloaded\n");
  }

  if (reset_notes && options.add_metadata_notes) {
    fastImport.write("reset refs/notes/commits\nfrom :" +
      to_string(maxMark + 1) +
      "\n");
  }
  }

int Repository::markFrom(const std::string &branchFrom, int branchRevNum, std::string &branchFromDesc)
  {
  Q_ASSERT(boost::starts_with(branchFrom,"refs/"));

  Branch &brFrom = branches[branchFrom];
  if (brFrom.lastChangeRev == Branch::neverChanged)
      return -1;

  if (brFrom.commits.isEmpty()) {
    return -1;
  }
  if (branchRevNum == brFrom.commits.last()) {
    return brFrom.marks.last();
  }

  QVector<int>::const_iterator it = qUpperBound(brFrom.commits, branchRevNum);
  if (it == brFrom.commits.begin()) {
    // Note: this warning is redundant with others, but it might be important to make sure we find out about this at the point it happened
    Log::warn() << "No mark found for r" << branchRevNum << " of branch "
                << branchFrom << " in repository " << name << std::endl;
    return 0;
  }

  int closestCommit = *--it;

  if (!branchFromDesc.empty()) {
    branchFromDesc += " at r" + to_string(branchRevNum);
    if (closestCommit != branchRevNum) {
      branchFromDesc += " => r" + to_string(closestCommit);
    }
  }

  return brFrom.marks[it - brFrom.commits.begin()];
  }

int Repository::createBranch(
    BranchRule const* branch_rule,
    int revnum,
    const std::string &branchFrom,
    int branchRevNum)
  {
  std::string branch = git_ref_name(branch_rule);
  
  Q_ASSERT(boost::starts_with(branch, "refs/"));
  Q_ASSERT(boost::starts_with(branchFrom, "refs/"));
  std::string branchFromDesc = "from branch " + branchFrom;
  int mark = markFrom(branchFrom, branchRevNum, branchFromDesc);

  if (mark == -1)
    {
    std::stringstream message;
    message << branch << " in repository " << name
            << " is branching from branch " << branchFrom
            << " but the latter doesn't exist. Can't continue.";
    throw std::runtime_error(message.str());
    }
  std::string branchFromRef = ":" + to_string(mark);
  if (!mark)
    {
    Log::warn() << branch << " in repository "
                << name << " is branching but no exported commits exist in repository."
                << " creating an empty branch." << std::endl;
    branchFromRef = branchFrom;
    branchFromDesc += ", deleted/unknown";
    }
  Log::debug() << "Creating branch: " << branch << " from "
               << branchFrom << " (r" << branchRevNum << ' '
               << branchFromDesc << ')' << " in repository "
               << name << std::endl;
  // Preserve note
  branches[branch].note = branches.value(branchFrom).note;
  return resetBranch(branch_rule, branch, revnum, mark, branchFromRef, branchFromDesc);
  }

int Repository::deleteBranch(BranchRule const* branch_rule, int revnum)
  {
  std::string branch = git_ref_name(branch_rule);
  Q_ASSERT(boost::starts_with(branch, "refs/"));

  if (branch == "refs/heads/master")
      return EXIT_SUCCESS;

  static std::string null_sha(40, '0');
  return resetBranch(branch_rule, branch, revnum, 0, null_sha, "delete");
  }

int Repository::resetBranch(
    BranchRule const* branch_rule,
    const std::string &gitRefName,  // Redundant with the above, but we've already computed it
    int revnum,
    int mark,                   // will be zero when deleting the branch
    const std::string &resetTo,
    const std::string &comment)
  {
  if (submodule_in_repo)
      submodule_in_repo->submoduleChanged(this, branch_rule, mark, revnum);
  
  Q_ASSERT(boost::starts_with(gitRefName, "refs/"));
  bool deleting = mark == 0;
  
  Branch &br = branches[gitRefName];
  std::string backupCmd;
  if (br.exists() && br.lastChangeRev != revnum)
    {
    std::string backupBranch;
    if (deleting && boost::starts_with(gitRefName, "refs/heads/"))
      {
      backupBranch = "refs/tags/backups/" + gitRefName.substr(11) + "@" + to_string(revnum);
      }
    else
      {
      backupBranch = "refs/backups/r" + to_string(revnum) + gitRefName.substr(4);
      }
    Log::debug() << "backing up branch " << gitRefName << " to "
                 << backupBranch << " in repository " << name
                 << std::endl;
    backupCmd = "reset " + backupBranch + "\nfrom " + gitRefName + "\n\n";
    }

  // When a branch is deleted, it gets a commit mark of zero
  br.lastChangeRev = revnum;
  br.commits.append(revnum);
  br.marks.append(mark);

  std::string cmd = "reset " + gitRefName + "\nfrom " + resetTo + "\n\n"
    "progress SVN r" + to_string(revnum)
    + " branch " + gitRefName + " = :" + to_string(mark)
    + " # " + comment + "\n\n";

  if (deleting)
    {
    // In a single revision, we can create a branch after deleting it,
    // but not vice-versa.  Just ignore both the deletion and the
    // original creation if they occur in the same revision.
    if (resetBranches.contains(gitRefName))
        resetBranches.remove(gitRefName);
    else
        deletedBranches[gitRefName].append(backupCmd).append(cmd);
    }
  else
    {
    resetBranches[gitRefName].append(backupCmd).append(cmd);
    }
  
  return EXIT_SUCCESS;
  }

void Repository::prepare_commit(int revnum)
  {
  if (deletedBranches.isEmpty() && resetBranches.isEmpty())
    {
    return;
    }

  BOOST_FOREACH(std::string const& branch_name, branches.keys())
    {
    Branch const& b = branches[branch_name];
    if (b.lastSubmoduleListChangeRev == revnum)
      {
      update_dot_gitmodules(branch_name, b, revnum);
      }
    }
  startFastImport();
  foreach(std::string const& cmd, deletedBranches.values())
    fastImport.write(cmd);
  foreach(std::string const& cmd, resetBranches.values())
    fastImport.write(cmd);
  deletedBranches.clear();
  resetBranches.clear();
  }

void Repository::commit(
    std::string const& author, uint epoch, std::string const& log)
  {
  BOOST_FOREACH(TransactionMap::reference t, transactions)
    {
    Transaction* txn = &t.second;
    txn->setAuthor(author);
    txn->setDateTime(epoch);
    txn->setLog(log);
    txn->commit();
    }
  transactions.clear();
  next_file_mark = maxMark;
  }

Repository::Transaction *Repository::demandTransaction(
    BranchRule const* branch,
    const std::string &svnprefix,
    int revnum)
  {
  return demandTransaction(git_ref_name(branch), svnprefix, revnum);
  }

Repository::Transaction *Repository::demandTransaction(
    const std::string &branch,
    const std::string &svnprefix,
    int revnum)
  {
  std::map<std::string,Transaction>::iterator
    p = transactions.find(branch);
  
  if (p != transactions.end())
    return &p->second;

  Transaction *txn = &transactions[branch];
  
  Q_ASSERT(boost::starts_with(branch, "refs/"));
  if (!branches.contains(branch))
    {
    Log::debug() << "Creating branch '" << branch << "' in repository '"
                 <<  name << "'." << std::endl;
    }

  txn->repository = this;
  txn->branch = branch;
  txn->svnprefix = svnprefix;
  txn->datetime = 0;
  txn->revnum = revnum;

  if ((++commitCount % options.commit_interval) == 0)
    {
    startFastImport();
    // write everything to disk every 10000 commits
    fastImport.write("checkpoint\n");
    Log::debug() << "checkpoint!, marks file trunkated" << std::endl;
    }
  return txn;
  }

void Repository::createAnnotatedTag(
    BranchRule const* branch_rule,
    const std::string &svnprefix,
    int revnum,
    const std::string &author,
    uint dt,
    const std::string &log)
  {
  std::string ref = git_ref_name(branch_rule);
  std::string tagName = ref;
  if (boost::starts_with(tagName, "refs/tags/"))
    {
    tagName.erase(0, 10);
    }
  if (!annotatedTags.contains(tagName))
    {
    Log::debug() << "Creating annotated tag " << tagName
                 << " (" << ref << ')' << " in repository "
                 << name << std::endl;
    }
  else
    {
    Log::debug() << "Re-creating annotated tag " << tagName
                 << " in repository " << name << std::endl;
    }
  AnnotatedTag &tag = annotatedTags[tagName];
  tag.supportingRef = ref;
  tag.svnprefix = svnprefix;
  tag.revnum = revnum;
  tag.author = author;
  tag.log = log;
  tag.dt = dt;
  }

void Repository::finalizeTags()
  {
  if (annotatedTags.isEmpty())
    {
    return;
    }
  std::ostream& output = Log::debug() << "Finalising tags for " << name << "...";
  startFastImport();

  QHash<std::string, AnnotatedTag>::ConstIterator it = annotatedTags.constBegin();
  for ( ; it != annotatedTags.constEnd(); ++it) {
    const std::string &tagName = it.key();
    const AnnotatedTag &tag = it.value();

    Q_ASSERT(boost::starts_with(tag.supportingRef, "refs/"));
    std::string message = tag.log;
    if (!boost::ends_with(message, "\n"))
        message += '\n';
    if (options.add_metadata)
        message += "\n" + formatMetadataMessage(tag.svnprefix, tag.revnum, tagName);

    {
    std::string const& branchRef = tag.supportingRef;

    uint msg_len = message.size();
    std::string s = "progress Creating annotated tag " + tagName + " from ref " + branchRef + "\n"
      + "tag " + tagName + "\n"
      + "from " + branchRef + "\n"
      + "tagger " + tag.author + ' ' + to_string(tag.dt) + " +0000" + "\n"
      + "data " + to_string( msg_len ) + "\n";
    fastImport.write(s);
    }

    fastImport.write(message.c_str(), message.size());
    fastImport.putChar('\n');
    if (!fastImport.waitForBytesWritten(-1))
        qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));

    // Append note to the tip commit of the supporting ref. There is no
    // easy way to attach a note to the tag itself with fast-import.
    if (options.add_metadata_notes) {
      Repository::Transaction *txn = demandTransaction(tag.supportingRef, tag.svnprefix, tag.revnum);
      txn->setAuthor(tag.author);
      txn->setDateTime(tag.dt);
      txn->commitNote(formatMetadataMessage(tag.svnprefix, tag.revnum, tagName), true);
      transactions.erase(tag.supportingRef);
      if (transactions.size() == 0)
          next_file_mark = maxMark;

      if (!fastImport.waitForBytesWritten(-1))
          qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));
    }

    output << ' ' << tagName << std::flush;
  }

  while (fastImport.bytesToWrite())
      if (!fastImport.waitForBytesWritten(-1))
          qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));
  output << std::endl;
  }

void Repository::startFastImport()
  {
  processCache.touch(this);

  if (fastImport.state() == QProcess::NotRunning) {
    if (processHasStarted)
        qFatal("git-fast-import has been started once and crashed?");
    processHasStarted = true;

    // start the process
    QString marksFile = marksFileName(name);
    QStringList marksOptions;
    marksOptions << "--import-marks=" + marksFile;
    marksOptions << "--export-marks=" + marksFile;
    marksOptions << "--force";

    fastImport.setStandardOutputFile(logFileName(name), QIODevice::Append);
    fastImport.setProcessChannelMode(QProcess::MergedChannels);

    if (!options.dry_run) {
      fastImport.start("git", QStringList() << "fast-import" << marksOptions);
    } else {
      fastImport.start("/bin/cat", QStringList());
    }
    fastImport.waitForStarted(-1);

    reloadBranches();
  }
  }

std::string Repository::formatMetadataMessage(const std::string &svnprefix, int revnum, const std::string &tag)
  {
  std::string msg = "svn path=" + svnprefix + "; revision=" + to_string(revnum);
  if (!tag.empty())
      msg += "; tag=" + std::string(tag.data(), tag.length());
  msg += "\n";
  return msg;
  }

bool Repository::branchExists(const std::string& branch) const
  {
  return branches.contains(branch);
  }

const std::string Repository::branchNote(const std::string& branch) const
  {
  return branches.value(branch).note;
  }

void Repository::setBranchNote(const std::string& branch, const std::string& noteText)
  {
  if (branches.contains(branch))
      branches[branch].note = noteText;
  }

void Repository::Transaction::setAuthor(const std::string &a)
  {
  author = a;
  }

void Repository::Transaction::setDateTime(uint dt)
  {
  datetime = dt;
  }

void Repository::Transaction::setLog(const std::string &l)
  {
  log = l;
  }

void Repository::Transaction::noteCopyFromBranch(
    const std::string &branchFrom,
    int branchRevNum)
  {
  Q_ASSERT(boost::starts_with(branchFrom, "refs/"));
  if (branch == branchFrom)
    {
    Log::warn() << "Cannot merge inside a branch" << " in repository " << repository->name << std::endl;
    return;
    }

  static std::string dummy;
  int mark = repository->markFrom(branchFrom, branchRevNum, dummy);
  Q_ASSERT(dummy.empty());

  if (mark == -1)
    {
    Log::warn() << branch << " is copying from branch " << branchFrom
                << " but the latter doesn't exist. Continuing, assuming the files exist"
                << " in repository " << repository->name << std::endl;
    }
  else if (mark == 0)
    {
    Log::warn() << "Unknown revision r" << branchRevNum << ". Continuing, assuming the files exist"
                << " in repository " << repository->name << std::endl;
    }
  else
    {
    Log::debug() << "repository " << repository->name << " branch " << branch
                 << " has some files copied from " << branchFrom << "@" << branchRevNum << std::endl;
    if (!merges.contains(mark))
      {
      merges.append(mark);
      Log::debug() << "adding " << branchFrom << "@" << branchRevNum << " : " << mark
                   << " as a merge point" << " in repository " << repository->name << std::endl;
      }
    else
      {
      Log::debug() << "merge point already recorded" << " in repository "
                   << repository->name << std::endl;
      }
    }
  }

void Repository::Transaction::deleteFile(const std::string &path)
  {
  std::string pathNoSlash = repository->prefix + path;
  if(boost::ends_with(pathNoSlash, "/"))
      pathNoSlash.erase(pathNoSlash.size() - 1);
  deletedFiles.append(pathNoSlash);
  }

QIODevice *Repository::Transaction::addFile(const std::string &path, int mode, qint64 length)
  {
  int mark = repository->next_file_mark--;

  // in case the two mark allocations meet, we might as well just abort
  Q_ASSERT(mark > repository->last_commit_mark + 1);
  
  Q_ASSERT(!(repository->prefix + path).empty());

  if (modifiedFiles.capacity() == 0)
      modifiedFiles.reserve(2048);
  modifiedFiles.append("M ");
  std::stringstream mode_stream;
  mode_stream << std::oct << mode;
  modifiedFiles.append(mode_stream.str());
  modifiedFiles.append(" :");
  modifiedFiles.append(to_string(mark));
  modifiedFiles.append(" ");
  modifiedFiles.append(repository->prefix + path);
  modifiedFiles.append("\n");

  // If it's not a submodule change, we have a blob to write.
  if (!options.dry_run) {
    repository->startFastImport();
    repository->fastImport.writeNoLog("blob\nmark :");
    repository->fastImport.writeNoLog(to_string(mark));
    repository->fastImport.writeNoLog("\ndata ");
    repository->fastImport.writeNoLog(to_string(length));
    repository->fastImport.writeNoLog("\n", 1);
  }

  return &repository->fastImport;
  }

void Repository::Transaction::commitNote(const std::string &noteText, bool append, const std::string &commit)
  {
  Q_ASSERT(boost::starts_with(branch, "refs/"));
  std::string branchRef = branch;
  const std::string &commitRef = commit.empty() ? branchRef : commit;
  std::string message = "Adding Git note for current " + commitRef + "\n";
  std::string text(noteText.data(), noteText.length());

  if (append && commit.empty() &&
    repository->branchExists(branch) &&
    !repository->branchNote(branch).empty())
    {
    text = repository->branchNote(branch) + text;
    message = "Appending Git note for current " + commitRef + "\n";
    }

  std::string s("");
  s.append("commit refs/notes/commits\n");
  s.append("mark :" + to_string(maxMark + 1) + "\n");
  s.append("committer " + author + " " + to_string(datetime) + " +0000" + "\n");
  s.append("data " + to_string(message.length()) + "\n");
  s.append(message + "\n");
  s.append("N inline " + commitRef + "\n");
  s.append("data " + to_string(text.length()) + "\n");
  s.append(text + "\n");
  repository->fastImport.write(s);

  if (commit.empty()) {
    repository->setBranchNote(branch, text);
  }
  }

void Repository::Transaction::commit()
  {
  repository->startFastImport();

  // We might be tempted to use the SVN revision number as the fast-import commit mark.
  // However, a single SVN revision can modify multple branches, and thus lead to multiple
  // commits in the same repo.  So, we need to maintain a separate commit mark counter.
  int mark = ++repository->last_commit_mark;

  // in case the two mark allocations meet, we might as well just abort
  Q_ASSERT(mark < repository->next_file_mark - 1);

  // create the commit message
  std::string message = log;
  if (!boost::ends_with(message, "\n"))
      message += '\n';
  if (options.add_metadata)
      message += "\n" + Repository::formatMetadataMessage(svnprefix, revnum);

  int parentmark = 0;
  Branch &br = repository->branches[branch];
  if (br.exists()) {
    parentmark = br.marks.last();
  } else {
    if (repository->incremental)
      {
      Log::warn() << "Branch " << branch << " in repository "
                  << repository->name << " doesn't exist at revision "
                  << revnum << " -- did you resume from the wrong revision?" << std::endl;
      }
  }
  br.lastChangeRev = revnum;
  br.commits.append(revnum);
  br.marks.append(mark);

  Q_ASSERT(boost::starts_with(branch, "refs/"));
  std::string branchRef = branch;

  std::string s("");
  s.append("commit " + branchRef + "\n");
  s.append("mark :" + to_string(mark) + "\n");
  s.append("committer " + author + " " + to_string(datetime) + " +0000" + "\n");
  s.append("data " + to_string(message.length()) + "\n");
  s.append(std::string(message.c_str()) + "\n");
  repository->fastImport.write(s);

  // note some of the inferred merges
  std::string desc = "";
  int i = !!parentmark;	// if parentmark != 0, there's at least one parent

  if(log.find("This commit was manufactured by cvs2svn") != std::string::npos && merges.count() > 1) {
    qSort(merges);
    repository->fastImport.write("merge :" + to_string(merges.last()) + "\n");
    merges.pop_back();
    Log::debug() << "Discarding all but the highest merge point "
                 << "as a workaround for cvs2svn created branch/tag. "
      //<< "Discarded marks: " << merges
      ;
  } else {
    foreach (const int merge, merges) {
      if (merge == parentmark) {
        Log::debug() << "Skipping marking " << merge << " as a merge point as it matches the parent"
                     << " in repository " << repository->name << std::endl;
        continue;
      }

      if (++i > 16) {
        // FIXME: options:
        //   (1) ignore the 16 parent limit
        //   (2) don't emit more than 16 parents
        //   (3) create another commit on branch to soak up additional parents
        // we've chosen option (2) for now, since only artificial commits
        // created by cvs2svn seem to have this issue
        Log::warn() << "too many merge parents" << " in repository "
                    << repository->name << std::endl;
        break;
      }

      std::string m = " :" + to_string(merge);
      desc += m;
      repository->fastImport.write("merge" + m + "\n");
    }
  }
  // write the file deletions
  if (deletedFiles.contains(""))
      repository->fastImport.write("deleteall\n");
  else
      foreach (std::string df, deletedFiles)
        repository->fastImport.write("D " + df + "\n");

  // write the file modifications
  repository->fastImport.write(modifiedFiles);

  repository->fastImport.write("\nprogress SVN r" + to_string(revnum)
    + " branch " + branch + " = :" + to_string(mark)
    + (desc.empty() ? "" : " # merge from") + desc
    + "\n\n");
  Log::trace() << deletedFiles.count() + boost::count(modifiedFiles, '\n')
               << " modifications from SVN " << svnprefix.data() << " to " << repository->name
               << '/' << branch.data() << std::endl;

  // Commit metadata note if requested
  if (options.add_metadata_notes)
      commitNote(Repository::formatMetadataMessage(svnprefix, revnum), false);

  while (repository->fastImport.bytesToWrite())
      if (!repository->fastImport.waitForBytesWritten(-1))
          qFatal("Failed to write to process: %s for repository %s",
            qPrintable(repository->fastImport.errorString()),
            repository->name.c_str());
  }

void Repository::submoduleChanged(
    Repository const* submodule, BranchRule const* branchRule, int submoduleMark, int revnum)
  {
  bool const deletion = submoduleMark == 0;
  std::string const& submodule_path = submodule->submodule_path;
  
  Branch& branch = branches[git_ref_name(branchRule)];

  if (deletion)
    {
    Branch::Submodules::iterator pos = branch.submodules.find(submodule_path);
    if (pos == branch.submodules.end())
        return; // If there's no submodule there already, don't bother with this change
    branch.submodules.erase(pos);
    }
  else
    {
    branch.submodules[submodule_path] = submodule;
    }
  
  std::ostream& debug = Log::debug();
  debug << "submodule " << submodule_path << " of repository " << this->name;
  if (deletion)
      debug << " deleted";
  else
      debug << " updated to mark :" << submoduleMark;
  debug << " in branch " << branchRule->name << " of r" << revnum << std::endl;

  Transaction* txn = demandTransaction(branchRule, std::string(), revnum);
  if (deletion)
      txn->deleteFile(submodule->submodule_path);
  else
      txn->updateSubmodule(submodule, submoduleMark);
  
  branch.lastSubmoduleListChangeRev = revnum;
  }

void Repository::Transaction::updateSubmodule(Repository const* submodule, int submoduleMark)
  {

  if (modifiedFiles.capacity() == 0)
      modifiedFiles.reserve(2048);
  modifiedFiles.append("M 160000 ");

  // Encode the submodule's mark in the place where its SHA belongs,
  // since we don't have the SHA for that commit at this point in the
  // process.  We'll take a second pass at this repository and fix up
  // all the submodule marks later.
  // 
  // We could do this in hex but we have enough digits to
  // transliterate it from decimal, and that will make debugging
  // easier
  std::stringstream fake_sha;
  fake_sha << /*std::hex <<*/ std::setfill('0') << std::setw(40) << submoduleMark;
  modifiedFiles.append(fake_sha.str());
  
  modifiedFiles.append(" ");
  modifiedFiles.append(submodule->submodule_path);
  modifiedFiles.append("\n");
  }

void Repository::update_dot_gitmodules(std::string const& branch_name, Branch const& b, int revnum)
  {
  Transaction* txn = demandTransaction(branch_name, "", revnum);
  std::stringstream content;
  
  for (Branch::Submodules::const_iterator p = b.submodules.begin(); p != b.submodules.end(); ++p)
    {
    content << "[submodule \"" << p->first << "\"]\n"
            << "	path = " << p->first << "\n"
            << "	url = http://github.com/boostorg/" << p->second->name << "\n"
      ;
    }

  QIODevice* device = txn->addFile(".gitmodules", 0100644, content.str().size());
  if (!options.dry_run)
    {
    device->write(content.str().c_str(), content.str().size());
    while (device->bytesToWrite() > 32 * 1024)
      {
      if (!device->waitForBytesWritten(-1))
        {
        std::stringstream msg;
        msg << "Failed to write to process: " << qPrintable(device->errorString());
        throw std::runtime_error(msg.str());
        }
      }
    // print an ending newline
    device->putChar('\n');
    }
  }
