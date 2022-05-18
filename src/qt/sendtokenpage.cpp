#include <qt/sendtokenpage.h>
#include <qt/forms/ui_sendtokenpage.h>

#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <validation.h>
#include <util/moneystr.h>
#include <qt/token.h>
#include <qt/bitcoinunits.h>
#include <wallet/wallet.h>
#include <validation.h>
#include <qt/guiutil.h>
#include <qt/sendcoinsdialog.h>
#include <qt/bitcoinaddressvalidator.h>
#include <uint256.h>
#include <qt/styleSheet.h>
#include <interfaces/node.h>
#include <qt/guiconstants.h>

//static const CAmount SINGLE_STEP = 0.00000001*COIN;

struct SelectedToken{
    std::string address;
    std::string sender;
    std::string symbol;
    int8_t decimals;
    std::string balance;
    SelectedToken():
        decimals(0)
    {}
};

SendTokenPage::SendTokenPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SendTokenPage),
    m_model(0),
    m_clientModel(0),
    m_tokenABI(0),
    m_selectedToken(0)
{
    // Setup ui components
    ui->setupUi(this);

    // Set stylesheet
    SetObjectStyleSheet(ui->clearButton, StyleSheetNames::ButtonBlack);

    ui->labelPayTo->setToolTip(tr("The address that will receive the tokens."));
    ui->labelAmount->setToolTip(tr("The amount in Token to send."));
    ui->labelDescription->setToolTip(tr("Optional description for transaction."));
    m_tokenABI = new Token();
    m_selectedToken = new SelectedToken();

    uint64_t blockGasLimit = 0;
    uint64_t minGasPrice = 0;
    uint64_t nGasPrice = 0;
    m_clientModel->getGasInfo(blockGasLimit, minGasPrice, nGasPrice);
    // Set defaults
    ui->lineEditGasPrice->setText(QString::number((double)(nGasPrice / LOC_GRANULARITY), 'f', 8));
    ui->lineEditGasLimit->setMaximum(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->lineEditGasLimit->setValue(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->confirmButton->setEnabled(false);

    // Connect signals with slots
    connect(ui->lineEditPayTo, SIGNAL(textChanged(QString)), SLOT(on_updateConfirmButton()));
    connect(ui->lineEditAmount, SIGNAL(valueChanged()), SLOT(on_updateConfirmButton()));
    connect(ui->confirmButton, SIGNAL(clicked()), SLOT(on_confirmClicked()));

    ui->lineEditPayTo->setCheckValidator(new BitcoinAddressCheckValidator(parent, true));
}

SendTokenPage::~SendTokenPage()
{
    delete ui;

    if(m_tokenABI)
        delete m_tokenABI;
    m_tokenABI = 0;
}

void SendTokenPage::setModel(WalletModel *_model)
{
    m_model = _model;
    m_tokenABI->setModel(m_model);
}

void SendTokenPage::setClientModel(ClientModel *_clientModel)
{
    m_clientModel = _clientModel;

    if (m_clientModel)
    {
        connect(m_clientModel, SIGNAL(gasInfoChanged(quint64, quint64, quint64)), this, SLOT(on_gasInfoChanged(quint64, quint64, quint64)));
    }
}

void SendTokenPage::clearAll()
{
    ui->lineEditPayTo->setText("");
    ui->lineEditAmount->clear();
    ui->lineEditDescription->setText("");
    ui->lineEditGasLimit->setValue(DEFAULT_GAS_LIMIT_OP_SEND);
}

bool SendTokenPage::isValidAddress()
{
    ui->lineEditPayTo->checkValidity();
    return ui->lineEditPayTo->isValid();
}

bool SendTokenPage::isDataValid()
{
    bool dataValid = true;

    if(!isValidAddress())
        dataValid = false;
    if(!ui->lineEditAmount->validate())
        dataValid = false;
    if(ui->lineEditAmount->value(0) <= 0)
    {
        ui->lineEditAmount->setValid(false);
        dataValid = false;
    }
    return dataValid;
}

void SendTokenPage::on_clearButton_clicked()
{
    clearAll();
}

void SendTokenPage::on_gasInfoChanged(quint64 blockGasLimit, quint64 minGasPrice, quint64 nGasPrice)
{
    if(m_clientModel)
    {
        uint64_t blockGasLimit = 0;
        uint64_t minGasPrice = 0;
        uint64_t nGasPrice = 0;
        m_clientModel->getGasInfo(blockGasLimit, minGasPrice, nGasPrice);

        ui->labelGasLimit->setToolTip(tr("Gas limit: Default = %1, Max = %2.").arg(DEFAULT_GAS_LIMIT_OP_SEND).arg(blockGasLimit));
        ui->labelGasPrice->setToolTip(tr("Gas price: HYDRA price per gas unit. Default = %1, Min = %2.").arg(QString::fromStdString(FormatMoney(DEFAULT_GAS_PRICE))).arg(QString::fromStdString(FormatMoney(minGasPrice))));
        ui->lineEditGasPrice->setText(QString::number((double)(nGasPrice / LOC_GRANULARITY), 'f', 8));
        ui->lineEditGasLimit->setMaximum(blockGasLimit);
    }
}

void SendTokenPage::on_updateConfirmButton()
{
    bool enabled = true;
    if(ui->lineEditPayTo->text().isEmpty() || ui->lineEditAmount->text().isEmpty())
    {
        enabled = false;
    }

    ui->confirmButton->setEnabled(enabled);
}

void SendTokenPage::on_confirmClicked()
{
    if(!isDataValid())
        return;

    WalletModel::UnlockContext ctx(m_model->requestUnlock());
    if(!ctx.isValid())
    {
        return;
    }

    if(m_model && m_model->wallet().isUnspentAddress(m_selectedToken->sender))
    {
        int unit = m_model->getOptionsModel()->getDisplayUnit();
        uint64_t gasLimit = ui->lineEditGasLimit->value();
        CAmount gasPrice = 666;
        std::string label = ui->lineEditDescription->text().trimmed().toStdString();

        m_tokenABI->setAddress(m_selectedToken->address);
        m_tokenABI->setSender(m_selectedToken->sender);
        m_tokenABI->setGasLimit(QString::number(gasLimit).toStdString());
        m_tokenABI->setGasPrice(BitcoinUnits::format(unit, gasPrice, false, BitcoinUnits::separatorNever).toStdString());

        std::string toAddress = ui->lineEditPayTo->text().toStdString();
        std::string amountToSend = ui->lineEditAmount->text().toStdString();
        QString amountFormated = BitcoinUnits::formatToken(m_selectedToken->decimals, ui->lineEditAmount->value(), false, BitcoinUnits::separatorAlways);

        QString questionString = tr("Are you sure you want to send? <br /><br />");
        questionString.append(tr("<b>%1 %2 </b> to ")
                              .arg(amountFormated).arg(QString::fromStdString(m_selectedToken->symbol)));
        questionString.append(tr("<br />%3 <br />")
                              .arg(QString::fromStdString(toAddress)));

        SendConfirmationDialog confirmationDialog(tr("Confirm send token."), questionString, 3, this);
        confirmationDialog.exec();
        QMessageBox::StandardButton retval = (QMessageBox::StandardButton)confirmationDialog.result();
        if(retval == QMessageBox::Yes)
        {
            bool success;
            if(m_tokenABI->transfer(toAddress, amountToSend, success, true))
            {
                interfaces::TokenTx tokenTx;
                tokenTx.contract_address = m_selectedToken->address;
                tokenTx.sender_address = m_selectedToken->sender;
                tokenTx.receiver_address = toAddress;
                dev::u256 nValue(amountToSend);
                tokenTx.value = u256Touint(nValue);
                tokenTx.tx_hash = uint256S(m_tokenABI->getTxId());
                tokenTx.label = label;
                m_model->wallet().addTokenTxEntry(tokenTx);
            }
            clearAll();
        }
    }
    else
    {
        QString message = tr("To send %1 you need HYDRA on address <br /> %2.")
                .arg(QString::fromStdString(m_selectedToken->symbol)).arg(QString::fromStdString(m_selectedToken->sender));

        QMessageBox::warning(this, tr("Send token"), message);
    }
}

void SendTokenPage::setTokenData(std::string address, std::string sender, std::string symbol, int8_t decimals, std::string balance)
{
    // Update data with the current token
    int decimalDiff = decimals - m_selectedToken->decimals;
    m_selectedToken->address = address;
    m_selectedToken->sender = sender;
    m_selectedToken->symbol = symbol;
    m_selectedToken->decimals = decimals;
    m_selectedToken->balance = balance;

    // Convert values for different number of decimals
    int256_t totalSupply(balance);
    int256_t value(ui->lineEditAmount->value());
    if(value != 0)
    {
        for(int i = 0; i < decimalDiff; i++)
        {
            value *= 10;
        }
        for(int i = decimalDiff; i < 0; i++)
        {
            value /= 10;
        }
    }
    if(value > totalSupply)
    {
        value = totalSupply;
    }

    // Update the amount field with the current token data
    ui->lineEditAmount->clear();
    ui->lineEditAmount->setDecimalUnits(decimals);
    ui->lineEditAmount->setTotalSupply(totalSupply);
    if(value != 0)
    {
        ui->lineEditAmount->setValue(value);
    }
}
