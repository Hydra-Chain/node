// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_VotingPage_H
#define BITCOIN_QT_VotingPage_H

#include "walletmodel.h"

#include <QDialog>
#include <QMessageBox>
#include <QString>
#include <QTimer>
#include <QLineEdit>

class ClientModel;
class PlatformStyle;
class Dgp;
class ExecRPCCommand;

namespace Ui {
    class VotingPageDialog;
}

QT_BEGIN_NAMESPACE
class QUrl;
QT_END_NAMESPACE

/** Dialog for sending bitcoins */
class VotingPage : public QDialog
{
    Q_OBJECT

public:
    explicit VotingPage(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~VotingPage();

    void setClientModel(ClientModel *clientModel);
    void setModel(WalletModel *model);

    /** Set up the tab chain manually, as Qt messes up the tab chain by default in some cases (issue https://bugreports.qt-project.org/browse/QTBUG-10907).
     */
    QWidget *setupTabChain(QWidget *prev);

    bool checkInProgress();

public Q_SLOTS:
    void voteNo();
    void voteYes();
    void updateTabsAndLabels();
	void updateVoteInfo();

private:
    Ui::VotingPageDialog *ui;
    ClientModel *clientModel;
    WalletModel *model;

    const PlatformStyle *platformStyle;
    Dgp* dgp;
    bool voteInProgress;
    ExecRPCCommand* execRPCCommand;
    QTimer *voteForAgainstTimer;
    uint64_t nGasPrice;
    mutable int seconds;


private Q_SLOTS:
    void on_voteYesButton_clicked();
    void on_voteNoButton_clicked();
	void voteButton(bool isYesVote);

Q_SIGNALS:
    // Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);
};


#define SEND_CONFIRM_DELAY   3

class SendVotingConfirmationDialog : public QMessageBox
{
    Q_OBJECT

public:
	SendVotingConfirmationDialog(const QString &title, const QString &text, int secDelay = SEND_CONFIRM_DELAY, QWidget *parent = 0);
    int exec();
    void setText(const QString &text);
    QLineEdit*  locAmount;

private Q_SLOTS:
    void countDown();
    void updateYesButton();
    void customSlot(const QString &text);

private:
    QAbstractButton *yesButton;
    QTimer countDownTimer;
    int secDelay;

};

#endif // BITCOIN_QT_VotingPage_H
