// Copyright (c) 2011-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_WALLETFRAME_H
#define BITCOIN_QT_WALLETFRAME_H

#include <QFrame>
#include <QMap>

class BitcoinGUI;
class ClientModel;
class PlatformStyle;
class SendCoinsRecipient;
class WalletModel;
class WalletView;

QT_BEGIN_NAMESPACE
class QStackedWidget;
QT_END_NAMESPACE

/**
 * A container for embedding all wallet-related
 * controls into BitcoinGUI. The purpose of this class is to allow future
 * refinements of the wallet controls with minimal need for further
 * modifications to BitcoinGUI, thus greatly simplifying merges while
 * reducing the risk of breaking top-level stuff.
 */
class WalletFrame : public QFrame
{
    Q_OBJECT

public:
    explicit WalletFrame(const PlatformStyle *platformStyle, BitcoinGUI *_gui = 0);
    ~WalletFrame();

    void setClientModel(ClientModel *clientModel);

    void addWallet(WalletModel *walletModel);
    void setCurrentWallet(const QString& name);
    void removeWallet(const QString &name);
    void removeAllWallets();

    bool handlePaymentRequest(const SendCoinsRecipient& recipient);

    void showOutOfSyncWarning(bool fShow);
    bool isVoteInProgress();

Q_SIGNALS:
    /** Notify that the user has requested more information about the out-of-sync warning */
    void requestedSyncWarningInfo();

private:
    QStackedWidget *walletStack;
    BitcoinGUI *gui;
    ClientModel *clientModel;
    QMap<QString, WalletView*> mapWalletViews;

    bool bOutOfSync;

    const PlatformStyle *platformStyle;

public:
    WalletView *currentWalletView();

    void updateTabBar(WalletView* walletView = 0, int index = -1);

public Q_SLOTS:
    /** Switch to overview (home) page */
    void gotoOverviewPage();
    /** Switch to history (transactions) page */
    void gotoHistoryPage();
    /** Switch to receive coins page */
    void gotoReceiveCoinsPage();
    /** Switch to send coins page */
    void gotoSendCoinsPage(QString addr = "");
    /** Switch to voting page */
    void gotoVotingPage(QString addr = "");
    /** Switch to create contract page */
    void gotoCreateContractPage();
    /** Switch to send contract page */
    void gotoSendToContractPage();
    /** Switch to call contract page */
    void gotoCallContractPage();
    /** Switch to Send Token page */
    void gotoSendTokenPage();
    /** Switch to Receive Token page */
    void gotoReceiveTokenPage();
    /** Switch to Add Token page */
    void gotoAddTokenPage();
    /** Switch to stake page */
    void gotoStakePage();
    /** Switch to delegation page */
    void gotoDelegationPage();
    /** Switch to super staker page */
    void gotoSuperStakerPage();

    /** Show Sign/Verify Message dialog and switch to sign message tab */
    void gotoSignMessageTab(QString addr = "");
    /** Show Sign/Verify Message dialog and switch to verify message tab */
    void gotoVerifyMessageTab(QString addr = "");

    /** Encrypt the wallet */
    void encryptWallet(bool status);
    /** Backup the wallet */
    void backupWallet();
    /** Restore the wallet */
    void restoreWallet();
    /** Change encrypted wallet passphrase */
    void changePassphrase();
    /** Ask for passphrase to unlock wallet temporarily */
    void unlockWallet();
    /** Lock the wallet */
    void lockWallet();

    /** Show used sending addresses */
    void usedSendingAddresses();
    /** Show used receiving addresses */
    void usedReceivingAddresses();
    /** Pass on signal over requested out-of-sync-warning information */
    void outOfSyncWarningClicked();
    
    void pageChanged(int index);
};

#endif // BITCOIN_QT_WALLETFRAME_H
