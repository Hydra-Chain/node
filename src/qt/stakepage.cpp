#include <qt/stakepage.h>
#include <qt/forms/ui_stakepage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>
#include <interfaces/wallet.h>
#include <interfaces/node.h>
#include <qt/transactiondescdialog.h>
#include <qt/styleSheet.h>
#include <qt/transactionview.h>
#include <amount.h>

#include <miner.h>

#include <QSortFilterProxyModel>

Q_DECLARE_METATYPE(interfaces::WalletBalances)

StakePage::StakePage(const PlatformStyle *_platformStyle, QWidget *parent) :
        QWidget(parent),
        ui(new Ui::StakePage),
        clientModel(nullptr),
        walletModel(nullptr),
        platformStyle(_platformStyle),
        transactionView(0),
        m_subsidy(0),
        m_networkWeight(0),
        m_expectedAnnualROI(0)
{
    ui->setupUi(this);
    ui->checkStake->setEnabled(gArgs.GetBoolArg("-staking", DEFAULT_STAKE));
    ui->labelCheckStakeText->setText(gArgs.GetBoolArg("-staking", DEFAULT_STAKE) ? "Staking on" : "Staking off");
    transactionView = new TransactionView(platformStyle, this, true);
    ui->frameStakeRecords->layout()->addWidget(transactionView);
}

StakePage::~StakePage()
{
    delete ui;
}

void StakePage::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &StakePage::numBlocksChanged);
        int height = _clientModel->node().getNumBlocks();
        ui->labelHeight->setText(QString::number(height));
        m_subsidy = _clientModel->node().getBlockSubsidy(height);
        m_networkWeight = _clientModel->node().getNetworkStakeWeight();
        m_expectedAnnualROI = _clientModel->node().getEstimatedAnnualROI();
        updateNetworkWeight();
        updateAnnualROI();
    }
}

void StakePage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        transactionView->setModel(model);
        transactionView->chooseType(6);
        ui->checkStake->setChecked(model->wallet().getEnabledStaking());

        // Keep up to date with wallet
        interfaces::Wallet& wallet = model->wallet();
        interfaces::WalletBalances balances = wallet.getBalances();
        setBalance(balances);
        connect(model, &WalletModel::balanceChanged, this, &StakePage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &StakePage::updateDisplayUnit);
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void StakePage::setBalance(const interfaces::WalletBalances& balances)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    m_balances = balances;
    ui->labelAssets->setText(BitcoinUnits::formatWithUnit(unit, balances.balance, false, BitcoinUnits::separatorAlways));
    ui->labelStake->setText(BitcoinUnits::formatWithUnit(unit, balances.stake, false, BitcoinUnits::separatorAlways));
}

void StakePage::on_checkStake_clicked(bool checked)
{
    if(!walletModel)
        return;

    this->walletModel->wallet().setEnabledStaking(checked);
    ui->labelCheckStakeText->setText(checked ? "Staking on" : "Staking off");

    if(checked && WalletModel::Locked == walletModel->getEncryptionStatus())
        Q_EMIT requireUnlock(true);
}

void StakePage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if (m_balances.balance != -1) {
            setBalance(m_balances);
        }
        updateSubsidy();
    }
}

void StakePage::numBlocksChanged(int count, const QDateTime &, double, bool headers)
{
    if(!headers && clientModel && walletModel)
    {
        ui->labelHeight->setText(BitcoinUnits::formatInt(count));
        m_subsidy = clientModel->node().getBlockSubsidy(count);
        m_networkWeight = clientModel->node().getNetworkStakeWeight();
        m_expectedAnnualROI = clientModel->node().getEstimatedAnnualROI();
        updateSubsidy();
        updateNetworkWeight();
        updateAnnualROI();
    }
}

void StakePage::updateSubsidy()
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    QString strSubsidy = BitcoinUnits::formatWithUnit(unit, m_subsidy, false, BitcoinUnits::separatorAlways) + "/Block";
    ui->labelReward->setText(strSubsidy);
}

void StakePage::updateNetworkWeight()
{
    ui->labelWeight->setText(BitcoinUnits::formatInt(m_networkWeight / COIN));
}

void StakePage::updateAnnualROI()
{
    ui->labelROI->setText(QString::number(m_expectedAnnualROI, 'f', 2) + "%");
}

void StakePage::updateEncryptionStatus()
{
    if(!walletModel)
        return;

    int status = walletModel->getEncryptionStatus();
    switch(status)
    {
    case WalletModel::Unlocked:
        if(walletModel->wallet().getEnabledStaking())
        {
            bool checked = ui->checkStake->isChecked();
            if(!checked) ui->checkStake->onStatusChanged();
        }
        break;
    case WalletModel::Locked:
        if(!walletModel->getWalletUnlockStakingOnly())
        {
            bool checked = ui->checkStake->isChecked();
            if(checked) ui->checkStake->onStatusChanged();
        }
        break;
    }
}