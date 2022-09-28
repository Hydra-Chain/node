#include <qt/sendtocontract.h>
#include <qt/forms/ui_sendtocontract.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/rpcconsole.h>
#include <qt/execrpccommand.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <validation.h>
#include <util/moneystr.h>
#include <qt/abifunctionfield.h>
#include <qt/contractutil.h>
#include <qt/tabbarinfo.h>
#include <qt/contractresult.h>
#include <qt/contractbookpage.h>
#include <qt/editcontractinfodialog.h>
#include <qt/contracttablemodel.h>
#include <qt/styleSheet.h>
#include <qt/guiutil.h>
#include <qt/sendcoinsdialog.h>
#include "locktrip/price-oracle.h"
#include <QClipboard>
#include <interfaces/node.h>

namespace SendToContract_NS
{
// Contract data names
static const QString PRC_COMMAND = "sendtocontract";
static const QString PARAM_ADDRESS = "address";
static const QString PARAM_DATAHEX = "datahex";
static const QString PARAM_AMOUNT = "amount";
static const QString PARAM_GASLIMIT = "gaslimit";
static const QString PARAM_SENDER = "sender";
}
using namespace SendToContract_NS;

SendToContract::SendToContract(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SendToContract),
    m_model(0),
    m_clientModel(0),
    m_contractModel(0),
    m_execRPCCommand(0),
    m_ABIFunctionField(0),
    m_contractABI(0),
    m_tabInfo(0),
    m_results(1)
{
    m_platformStyle = platformStyle;

    // Setup ui components
    Q_UNUSED(platformStyle);
    ui->setupUi(this);
    ui->saveInfoButton->setIcon(platformStyle->MultiStatesIcon(":/icons/filesave", PlatformStyle::PushButton));
    ui->loadInfoButton->setIcon(platformStyle->MultiStatesIcon(":/icons/address-book", PlatformStyle::PushButton));
    ui->pasteAddressButton->setIcon(platformStyle->MultiStatesIcon(":/icons/editpaste", PlatformStyle::PushButton));
    // Format tool buttons
    GUIUtil::formatToolButtons(ui->saveInfoButton, ui->loadInfoButton, ui->pasteAddressButton);

    // Set stylesheet
    SetObjectStyleSheet(ui->pushButtonClearAll, StyleSheetNames::ButtonBlack);

    m_ABIFunctionField = new ABIFunctionField(platformStyle, ABIFunctionField::SendTo, ui->scrollAreaFunction);
    ui->scrollAreaFunction->setWidget(m_ABIFunctionField);
    ui->lineEditAmount->setEnabled(true);
    ui->labelContractAddress->setToolTip(tr("The contract address that will receive the funds and data."));
    ui->labelAmount->setToolTip(tr("The amount in HYDRA to send. Default = 0."));
    ui->labelSenderAddress->setToolTip(tr("The HYDRA address that will be used as sender."));

    m_tabInfo = new TabBarInfo(ui->stackedWidget);
    m_tabInfo->addTab(0, tr("Send To Contract"));

    uint64_t blockGasLimit = 0;
    uint64_t minGasPrice = 0;
    uint64_t nGasPrice = 0;
    m_clientModel->getGasInfo(blockGasLimit, minGasPrice, nGasPrice);

    // Set defaults
    ui->lineEditGasPrice->setText(QString::number((double)(nGasPrice / LOC_GRANULARITY), 'f', 8));
    ui->lineEditGasLimit->setMinimum(MINIMUM_GAS_LIMIT);
    ui->lineEditGasLimit->setMaximum(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->lineEditGasLimit->setValue(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->textEditInterface->setIsValidManually(true);
    ui->pushButtonSendToContract->setEnabled(false);
    ui->lineEditSenderAddress->setSenderAddress(true);
    ui->lineEditSenderAddress->setComboBoxEditable(true);

    // Create new PRC command line interface
    QStringList lstMandatory;
    lstMandatory.append(PARAM_ADDRESS);
    lstMandatory.append(PARAM_DATAHEX);
    QStringList lstOptional;
    lstOptional.append(PARAM_AMOUNT);
    lstOptional.append(PARAM_GASLIMIT);
    lstOptional.append(PARAM_SENDER);
    QMap<QString, QString> lstTranslations;
    lstTranslations[PARAM_ADDRESS] = ui->labelContractAddress->text();
    lstTranslations[PARAM_AMOUNT] = ui->labelAmount->text();
    lstTranslations[PARAM_GASLIMIT] = ui->labelGasLimit->text();
    lstTranslations[PARAM_SENDER] = ui->labelSenderAddress->text();
    m_execRPCCommand = new ExecRPCCommand(PRC_COMMAND, lstMandatory, lstOptional, lstTranslations, this);
    m_contractABI = new ContractABI();

    // Connect signals with slots
    connect(ui->pushButtonClearAll, SIGNAL(clicked()), SLOT(on_clearAllClicked()));
    connect(ui->pushButtonSendToContract, SIGNAL(clicked()), SLOT(on_sendToContractClicked()));
    connect(ui->lineEditContractAddress, SIGNAL(textChanged(QString)), SLOT(on_updateSendToContractButton()));
    connect(ui->textEditInterface, SIGNAL(textChanged()), SLOT(on_newContractABI()));
    connect(ui->stackedWidget, SIGNAL(currentChanged(int)), SLOT(on_updateSendToContractButton()));
    connect(m_ABIFunctionField, SIGNAL(functionChanged()), SLOT(on_functionChanged()));
    connect(ui->saveInfoButton, SIGNAL(clicked()), SLOT(on_saveInfoClicked()));
    connect(ui->loadInfoButton, SIGNAL(clicked()), SLOT(on_loadInfoClicked()));
    connect(ui->pasteAddressButton, SIGNAL(clicked()), SLOT(on_pasteAddressClicked()));
    connect(ui->lineEditContractAddress, SIGNAL(textChanged(QString)), SLOT(on_contractAddressChanged()));

    // Set contract address validator
    QRegularExpression regEx;
    regEx.setPattern(paternAddress);
    QRegularExpressionValidator *addressValidatr = new QRegularExpressionValidator(ui->lineEditContractAddress);
    addressValidatr->setRegularExpression(regEx);
    ui->lineEditContractAddress->setCheckValidator(addressValidatr);
}

SendToContract::~SendToContract()
{
    delete m_contractABI;
    delete ui;
}

void SendToContract::setModel(WalletModel *_model)
{
    m_model = _model;
    m_contractModel = m_model->getContractTableModel();
    ui->lineEditSenderAddress->setWalletModel(m_model);
}

bool SendToContract::isValidContractAddress()
{
    ui->lineEditContractAddress->checkValidity();
    return ui->lineEditContractAddress->isValid();
}

bool SendToContract::isValidInterfaceABI()
{
    ui->textEditInterface->checkValidity();
    return ui->textEditInterface->isValid();
}

bool SendToContract::isDataValid()
{
    bool dataValid = true;

    if(!isValidContractAddress())
        dataValid = false;
    if(!isValidInterfaceABI())
        dataValid = false;
    if(!m_ABIFunctionField->isValid())
        dataValid = false;
    return dataValid;
}

void SendToContract::setContractAddress(const QString &address)
{
    ui->lineEditContractAddress->setText(address);
    ui->lineEditContractAddress->setFocus();
}

void SendToContract::setClientModel(ClientModel *_clientModel)
{
    m_clientModel = _clientModel;

    if (m_clientModel)
    {
        connect(m_clientModel, SIGNAL(gasInfoChanged(quint64, quint64, quint64)), this, SLOT(on_gasInfoChanged(quint64, quint64, quint64)));
    }
}

void SendToContract::on_clearAllClicked()
{
    uint64_t blockGasLimit = 0;
    uint64_t minGasPrice = 0;
    uint64_t nGasPrice = 0;
    m_clientModel->getGasInfo(blockGasLimit, minGasPrice, nGasPrice);

    ui->lineEditContractAddress->clear();
    ui->lineEditAmount->clear();
    ui->lineEditAmount->setEnabled(true);
    ui->lineEditGasLimit->setValue(DEFAULT_GAS_LIMIT_OP_SEND);
    ui->lineEditGasPrice->setText(QString::number((double)(nGasPrice / LOC_GRANULARITY), 'f', 8));
    ui->lineEditSenderAddress->setCurrentIndex(-1);
    ui->textEditInterface->clear();
    ui->textEditInterface->setIsValidManually(true);
    m_tabInfo->clear();
}

void SendToContract::on_sendToContractClicked()
{
    if(isDataValid())
    {
        WalletModel::UnlockContext ctx(m_model->requestUnlock());
        if(!ctx.isValid())
        {
            return;
        }

        // Initialize variables
        QMap<QString, QString> lstParams;
        QVariant result;
        QString errorMessage;
        QString resultJson;
        int unit = m_model->getOptionsModel()->getDisplayUnit();
        uint64_t gasLimit = ui->lineEditGasLimit->value();
        int func = m_ABIFunctionField->getSelectedFunction();

        // Append params to the list
        ExecRPCCommand::appendParam(lstParams, PARAM_ADDRESS, ui->lineEditContractAddress->text());
        ExecRPCCommand::appendParam(lstParams, PARAM_DATAHEX, toDataHex(func, errorMessage));
        QString amount = isFunctionPayable() ? BitcoinUnits::format(unit, ui->lineEditAmount->value(), false, BitcoinUnits::separatorNever) : "0";
        ExecRPCCommand::appendParam(lstParams, PARAM_AMOUNT, amount);
        ExecRPCCommand::appendParam(lstParams, PARAM_GASLIMIT, QString::number(gasLimit));
        ExecRPCCommand::appendParam(lstParams, PARAM_SENDER, ui->lineEditSenderAddress->currentText());

        QString questionString = tr("Are you sure you want to send to the contract: <br /><br />");
        questionString.append(tr("<b>%1</b>?")
                              .arg(ui->lineEditContractAddress->text()));

        SendConfirmationDialog confirmationDialog(tr("Confirm sending to contract."), questionString, 3, this);
        confirmationDialog.exec();
        QMessageBox::StandardButton retval = (QMessageBox::StandardButton)confirmationDialog.result();
        if(retval == QMessageBox::Yes)
        {
            // Execute RPC command line
            if(errorMessage.isEmpty() && m_execRPCCommand->exec(m_model->node(), m_model->wallet(), lstParams, result, resultJson, errorMessage))
            {
                ContractResult *widgetResult = new ContractResult(ui->stackedWidget);
                widgetResult->setResultData(result, FunctionABI(), m_ABIFunctionField->getParamsValues(), ContractResult::SendToResult);
                ui->stackedWidget->addWidget(widgetResult);
                int position = ui->stackedWidget->count() - 1;
                m_results = position == 1 ? 1 : m_results + 1;

                m_tabInfo->addTab(position, tr("Result %1").arg(m_results));
                m_tabInfo->setCurrent(position);
            }
            else
            {
                QMessageBox::warning(this, tr("Send to contract"), errorMessage);
            }
        }
    }
}

void SendToContract::on_gasInfoChanged(quint64 blockGasLimit, quint64 minGasPrice, quint64 nGasPrice)
{
	if(m_clientModel)
    {
        uint64_t blockGasLimit = 0;
        uint64_t minGasPrice = 0;
        uint64_t nGasPrice = 0;
        m_clientModel->getGasInfo(blockGasLimit, minGasPrice, nGasPrice);

        ui->labelGasLimit->setToolTip(tr("Gas limit: Default = %1, Max = %2.").arg(DEFAULT_GAS_LIMIT_OP_SEND).arg(blockGasLimit));
        ui->lineEditGasLimit->setMaximum(blockGasLimit);
        ui->lineEditGasPrice->setText(QString::number((double)(nGasPrice / LOC_GRANULARITY), 'f', 8));
        ui->lineEditSenderAddress->on_refresh();
    }
}

void SendToContract::on_updateSendToContractButton()
{
    int func = m_ABIFunctionField->getSelectedFunction();
    bool enabled = func >= -1;
    if(ui->lineEditContractAddress->text().isEmpty())
    {
        enabled = false;
    }
    enabled &= ui->stackedWidget->currentIndex() == 0;

    ui->pushButtonSendToContract->setEnabled(enabled);
}

void SendToContract::on_newContractABI()
{
    std::string json_data = ui->textEditInterface->toPlainText().toStdString();
    if(!m_contractABI->loads(json_data))
    {
        m_contractABI->clean();
        ui->textEditInterface->setIsValidManually(false);
    }
    else
    {
        ui->textEditInterface->setIsValidManually(true);
    }
    m_ABIFunctionField->setContractABI(m_contractABI);

    on_updateSendToContractButton();
}

void SendToContract::on_functionChanged()
{
    bool payable = isFunctionPayable();
    ui->lineEditAmount->setEnabled(payable);
    if(!payable)
    {
        ui->lineEditAmount->clear();
    }
}

void SendToContract::on_saveInfoClicked()
{
    if(!m_contractModel)
        return;

    bool valid = true;

    if(!isValidContractAddress())
        valid = false;

    if(!isValidInterfaceABI())
        valid = false;

    if(!valid)
        return;

    QString contractAddress = ui->lineEditContractAddress->text();
    int row = m_contractModel->lookupAddress(contractAddress);
    EditContractInfoDialog::Mode dlgMode = row > -1 ? EditContractInfoDialog::EditContractInfo : EditContractInfoDialog::NewContractInfo;
    EditContractInfoDialog dlg(dlgMode, this);
    dlg.setModel(m_contractModel);
    if(dlgMode == EditContractInfoDialog::EditContractInfo)
    {
        dlg.loadRow(row);
    }
    dlg.setAddress(ui->lineEditContractAddress->text());
    dlg.setABI(ui->textEditInterface->toPlainText());
    if(dlg.exec())
    {
        ui->lineEditContractAddress->setText(dlg.getAddress());
        ui->textEditInterface->setText(dlg.getABI());
        on_contractAddressChanged();
    }
}

void SendToContract::on_loadInfoClicked()
{
    ContractBookPage dlg(m_platformStyle, this);
    dlg.setModel(m_model->getContractTableModel());
    if(dlg.exec())
    {
        ui->lineEditContractAddress->setText(dlg.getAddressValue());
        on_contractAddressChanged();
    }
}

void SendToContract::on_pasteAddressClicked()
{
    setContractAddress(QApplication::clipboard()->text());
}

void SendToContract::on_contractAddressChanged()
{
    if(isValidContractAddress() && m_contractModel)
    {
        QString contractAddress = ui->lineEditContractAddress->text();
        if(m_contractModel->lookupAddress(contractAddress) > -1)
        {
            QString contractAbi = m_contractModel->abiForAddress(contractAddress);
            if(ui->textEditInterface->toPlainText() != contractAbi)
            {
                ui->textEditInterface->setText(m_contractModel->abiForAddress(contractAddress));
            }
        }
    }
}

QString SendToContract::toDataHex(int func, QString& errorMessage)
{
    if(func == -1 || m_ABIFunctionField == NULL || m_contractABI == NULL)
    {
        std::string defSelector = FunctionABI::defaultSelector();
        return QString::fromStdString(defSelector);
    }

    std::string strData;
    std::vector<std::vector<std::string>> values = m_ABIFunctionField->getValuesVector();
    FunctionABI function = m_contractABI->functions[func];
    std::vector<ParameterABI::ErrorType> errors;
    if(function.abiIn(values, strData, errors))
    {
        return QString::fromStdString(strData);
    }
    else
    {
        errorMessage = ContractUtil::errorMessage(function, errors, true);
    }
    return "";
}

bool SendToContract::isFunctionPayable()
{
    int func = m_ABIFunctionField->getSelectedFunction();
    if(func < 0) return true;
    FunctionABI function = m_contractABI->functions[func];
    return function.payable;
}
