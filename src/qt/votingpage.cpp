// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "votingpage.h"
#include "ui_votingpage.h"

#include "clientmodel.h"
#include "guiutil.h"
#include "platformstyle.h"
#include "walletmodel.h"
#include "guiconstants.h"
#include "styleSheet.h"
#include "execrpccommand.h"
#include "contractabi.h"
#include "util.h"
#include "locktrip/price-oracle.h"

#include "ui_interface.h"
#include <locktrip/dgp.h>
#include "validation.h"
#include <QFontMetrics>
#include <QMessageBox>
#include <QTimer>
#include <QLineEdit>
#include <QSpacerItem>
#include <QDebug>
#include <QErrorMessage>


static const QString PRC_CALL = "sendtocontract";
static const QString PARAM_ADDRESS = "address";
static const QString PARAM_DATAHEX = "datahex";
static const QString PARAM_AMOUNT = "amount";
static const QString PARAM_GASLIMIT = "gaslimit";

static const uint64_t LOC_COIN = 100000000;
static const uint64_t VOTE_GAS_LIMIT = 50000;
static const QString VOTE_FINISHED = "Voting has just finished";



void VotingPage::updateVoteInfo() {
	uint64_t coinBurnPercentage = DGP_CACHE_BURN_RATE;
	voteInProgress = false;
	bool inProgress = dgp->hasVoteInProgress(voteInProgress);
	LogPrintf("hasVoteInProgress executes successfully(%d) returns inProgress = %d \n", voteInProgress, inProgress);
	dgp_currentVote gdpCurrentVote;
	bool ret = dgp->getCurrentVote(gdpCurrentVote);
	if (ret && inProgress && voteInProgress) {
		ui->voteYesButton->setEnabled(true);
		ui->voteNoButton->setEnabled(true);
		CBlockIndex* tip = chainActive.Tip();
		LogPrintf("start_block: %ld ; blocksExpiration: %ld ; Height: %d\n", gdpCurrentVote.start_block, gdpCurrentVote.blocksExpiration, chainActive.Height());
		CBlockIndex* startBlock = tip->GetAncestor(gdpCurrentVote.start_block);
		int64_t startBlock_timestamp = startBlock->GetBlockTime();
		QDateTime startBlock_qt_timestamp;
		startBlock_qt_timestamp.setTime_t(startBlock_timestamp);
		ui->labelVoteBeginsValue->setText(QString::number(gdpCurrentVote.start_block) + " / BlockTime: " + startBlock_qt_timestamp.toString(Qt::SystemLocaleLongDate));
		LogPrintf("startBlock_timestamp: %s \n", startBlock_qt_timestamp.toLocalTime().toString("hh:mm:ss").toStdString().data());
		int64_t endBlock = gdpCurrentVote.start_block + gdpCurrentVote.blocksExpiration;
		ui->labelVoteEndsValue->setText(QString::number(endBlock));
		ui->labelVoteDurationValue->setText(QString::number(gdpCurrentVote.blocksExpiration));
		LogPrintf("threshold: %ld\n", gdpCurrentVote.threshold);
		uint64_t locTresholdInSatoshis;
		dgp->convertFiatThresholdToLoc(gdpCurrentVote.threshold, locTresholdInSatoshis);
		ui->labelThresholdValue->setText(QString::number(locTresholdInSatoshis) + " votes");
		if(gdpCurrentVote.param == dgp_params::REMOVE_ADMIN_VOTE || gdpCurrentVote.param == dgp_params::ADMIN_VOTE) {
			ui->labelBurnRate->setText(QString::fromStdString(gdpCurrentVote.newAdmin.hex()));
		}
		else if(gdpCurrentVote.param == dgp_params::FIAT_GAS_PRICE || gdpCurrentVote.param == dgp_params::FIAT_BYTE_PRICE) {
			ui->labelBurnRate->setText(QString::number((double)(gdpCurrentVote.param_value) / (double)100, 'f', 2)  + '$');
		}
		else if(gdpCurrentVote.param == dgp_params::BURN_RATE || gdpCurrentVote.param == dgp_params::ECONOMY_DIVIDEND) {
			ui->labelBurnRate->setText(QString::number(gdpCurrentVote.param_value) + '%');
		}
		else if(gdpCurrentVote.param == dgp_params::BLOCK_SIZE_DGP_PARAM) {
			ui->labelBurnRate->setText(QString::number(gdpCurrentVote.param_value) + ' bytes');
		}
		else if(gdpCurrentVote.param == dgp_params::BLOCK_GAS_LIMIT_DGP_PARAM) {
			ui->labelBurnRate->setText(QString::number(gdpCurrentVote.param_value) + ' gas');
		}

		LogPrintf("coinBurnPercentage: %ld ; param_value: %ld ; param: %ld\n", coinBurnPercentage, gdpCurrentVote.param_value, gdpCurrentVote.param);
		ui->labelVotetoBurnStatic->setText(QString(VOTE_HEADLINES.begin()[gdpCurrentVote.param]));
		ui->labelIDVoteValue->setText(QString::number(gdpCurrentVote.param));
		ui->voteInitiatorvalue->setText(QString::fromStdString(gdpCurrentVote.vote_creator.hex()));
		LogPrintf("votesFor: %ld ; votesAgainst: %ld \n", gdpCurrentVote.votesFor, gdpCurrentVote.votesAgainst);
		if (gdpCurrentVote.votesFor + gdpCurrentVote.votesAgainst > 0) {
			int64_t againstPerc = gdpCurrentVote.votesAgainst * 100 / (gdpCurrentVote.votesFor + gdpCurrentVote.votesAgainst);
			ui->labelNoResultValue->setText(QString::number(againstPerc) + '%');
			ui->labelYesResultValue->setText(QString::number(100 - againstPerc) + '%');
		} else {
			ui->labelNoResultValue->setText(QString::number(0) + '%');
			ui->labelYesResultValue->setText(QString::number(0) + '%');
		}
		voteForAgainstTimer->stop();
		voteForAgainstTimer->disconnect();
		delete voteForAgainstTimer;
		voteForAgainstTimer = new QTimer(this);
		connect(voteForAgainstTimer, &QTimer::timeout, this,[=]() {
			dgp_currentVote gdpCurrentVoteTimer;
			if(dgp->getCurrentVote(gdpCurrentVoteTimer) && dgp->hasVoteInProgress(voteInProgress) && voteInProgress)
			{
				ui->labelTimeLeft->setText(QString::number(endBlock - chainActive.Height()));
				ui->CurrentBlockValue->setText(QString::number(chainActive.Height()));
				if(endBlock - chainActive.Height() < 10)
				{
					ui->labelTimeLeft->setStyleSheet("QLabel { color : red; }");
				}
				if((gdpCurrentVoteTimer.votesFor + gdpCurrentVoteTimer.votesAgainst) == 0)
				{
					ui->labelNoResultValue->setText(QString::number(0) + '%');
					ui->labelYesResultValue->setText(QString::number(0) + '%');
				}
				else
				{
					int64_t againstPercTimer = gdpCurrentVoteTimer.votesAgainst * 100 / (gdpCurrentVoteTimer.votesFor + gdpCurrentVoteTimer.votesAgainst);
					ui->labelNoResultValue->setText(QString::number(againstPercTimer) + '%');
					int64_t forPercTimer = 100 - againstPercTimer;
					ui->labelYesResultValue->setText(QString::number(forPercTimer) + '%');
					ui->labelYesCountValue->setText(QString::number(gdpCurrentVoteTimer.votesFor));
					ui->labelNoCountValue->setText(QString::number(gdpCurrentVoteTimer.votesAgainst));
					LogPrintf("againstPercTimer: %d ; forPercTimer: %d \n", againstPercTimer, forPercTimer);
				}

			}
			else
			{
				ui->voteInitiatorvalue->setText(VOTE_FINISHED);
				ui->labelIDVoteValue->setText(VOTE_FINISHED);
				ui->labelVoteBeginsValue->setText(VOTE_FINISHED);
				ui->labelVoteDurationValue->setText(VOTE_FINISHED);
				ui->labelVoteEndsValue->setText(VOTE_FINISHED);
				ui->CurrentBlockValue->setText(VOTE_FINISHED);
				ui->labelTimeLeft->setText(VOTE_FINISHED);
				ui->labelThresholdValue->setText(VOTE_FINISHED);
			}
		});
		LogPrintf("voteForAgainst Timer restarted\n");
		voteForAgainstTimer->start(1000);
	}
	else
	{
		voteForAgainstTimer->stop();
		ui->voteYesButton->setEnabled(false);
		ui->voteNoButton->setEnabled(false);
	}
}

VotingPage::VotingPage(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::VotingPageDialog),
    clientModel(0),
    model(0),
    platformStyle(_platformStyle),
	dgp(new Dgp()),
	nGasPrice(0)
{
	voteInProgress = false;
    ui->setupUi(this);

    voteForAgainstTimer = new QTimer(this);
    // Set stylesheet
    SetObjectStyleSheet(ui->voteNoButton, StyleSheetNames::ButtonBlack);
    SetObjectStyleSheet(ui->voteYesButton, StyleSheetNames::ButtonBlue);


    if (!_platformStyle->getImagesOnButtons()) {
        ui->voteYesButton->setIcon(QIcon());
    } else {
        ui->voteYesButton->setIcon(_platformStyle->MultiStatesIcon(":/icons/add_recipient", PlatformStyle::PushButton));
    }

    PriceOracle oracle;
    oracle.getPrice(nGasPrice);
    connect(ui->voteYesButton, SIGNAL(clicked()), this, SLOT(voteYes()));
    connect(ui->voteNoButton, SIGNAL(clicked()), this, SLOT(voteNo()));

	updateVoteInfo();

	QStringList lstMandatory;
	lstMandatory.append(PARAM_ADDRESS);
	lstMandatory.append(PARAM_DATAHEX);
	QStringList lstOptional;
	lstOptional.append(PARAM_AMOUNT);
    lstOptional.append(PARAM_GASLIMIT);
	execRPCCommand = new ExecRPCCommand(PRC_CALL, lstMandatory, lstOptional, QMap<QString, QString>());
}

void VotingPage::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;
}

VotingPage::~VotingPage()
{
	delete execRPCCommand;
	voteForAgainstTimer->stop();
    delete ui;
}

void VotingPage::voteButton(bool isYesVote) {
	QStringList formatted;
	QString questionString = tr("Please select the amount of LOCs to use for voting.");
	questionString.append("<br /><br />%1");
	questionString.append("<hr /><span style='color:#aa0000;'>");
	questionString.append("</span> ");
	questionString.append("Your vote weight will be proportional to the amount of LOCs spent.");

	questionString.append("<hr /><span style='color:#aa0000;'>");
	questionString.append("The LOCs used for voting will be burned and cannot be retrieved!");
	questionString.append("</span>");
	questionString.append("<hr /><span style='color:#03aa00;font-size: 12px;'>");
	questionString.append("Transaction fee (approximately): -" + QString::number((double)(VOTE_GAS_LIMIT * nGasPrice) / (double)LOC_COIN) + " LOCs" );
	questionString.append("</span>");

	SendVotingConfirmationDialog confirmationDialog(
			tr("The weight of your vote"), tr(""), SEND_CONFIRM_DELAY, this);
	confirmationDialog.setText(questionString.arg(formatted.join("<br />")));
	confirmationDialog.exec();
	QMessageBox::StandardButton retval =
			(QMessageBox::StandardButton) (confirmationDialog.result());
	qDebug() << "your choice was:" << retval;
	if (retval == QMessageBox::Yes) {
		QMessageBox::StandardButton reply;
		reply = QMessageBox::question(&confirmationDialog, "Confirm voting",
				"Voting will burn your " + confirmationDialog.locAmount->text() + " LOC(s)\n\n Are you sure?",
				QMessageBox::Yes | QMessageBox::No);
		if (reply == QMessageBox::Yes) {
			qDebug() << "Yes was clicked";

	        QVariant result;
	        QString errorMessage;
	        QString resultJson;
	        QMap<QString, QString> lstParams;
	        std::string strData;
			FunctionABI function = dgp->m_contractAbi.functions[dgp_contract_funcs::VOTE];
			std::vector<std::vector<std::string>> values;
			std::vector<std::string> param;
			param.push_back(isYesVote ? "true" : "false");
			values.push_back(param);
			std::vector<ParameterABI::ErrorType> errors;
			if(!function.abiIn(values, strData, errors))
				return;
			LogPrintf("strData: %s\n", strData.data());
			QString strFuncData = QString::fromStdString(strData);
			ExecRPCCommand::appendParam(lstParams, PARAM_ADDRESS, QString::fromStdString(LockTripDgpContract.hex()));
	        ExecRPCCommand::appendParam(lstParams, PARAM_DATAHEX, strFuncData);
	        double LOCs = confirmationDialog.locAmount->text().toDouble();
	        LogPrintf("LOCs: %ul\n", confirmationDialog.locAmount->text().toDouble());
	        ExecRPCCommand::appendParam(lstParams, PARAM_AMOUNT, QString::number(LOCs, 'f', 9));
	        ExecRPCCommand::appendParam(lstParams, PARAM_GASLIMIT, QString::number(VOTE_GAS_LIMIT));
		    bool smartContractSucceed = execRPCCommand->exec(model->node(), model->wallet(), lstParams, result, resultJson, errorMessage);
		    if(!errorMessage.isEmpty())
		    {
		    	QMessageBox::critical( this, tr("Voting failed"), tr("Reason: %1\n").arg(errorMessage));
		    }
		    LogPrintf("smartContractSucceed: %d \n; result: %s\n; errorMessage: %s\n", smartContractSucceed, resultJson.toStdString().data(),errorMessage.toStdString().data());
		} else {
			qDebug() << "Yes was *not* clicked";
		}
	}
}

void VotingPage::on_voteYesButton_clicked()
{
    if(!model || !model->getOptionsModel())
    {
        return;
    }
	voteButton(true);
}


void VotingPage::on_voteNoButton_clicked()
{
    if(!model || !model->getOptionsModel())
    {
        return;
    }
	voteButton(false);
}

void VotingPage::voteYes()
{
	LogPrintf("voteYes\n");
	updateVoteInfo();
    updateTabsAndLabels();
}

void VotingPage::voteNo()
{
	LogPrintf("voteNo\n");
	updateVoteInfo();
    updateTabsAndLabels();
}

void VotingPage::setModel(WalletModel *_model)
{
    this->model = _model;
}

bool VotingPage::checkInProgress()
{
	bool voteInProgress = false;
	dgp->hasVoteInProgress(voteInProgress);
	return voteInProgress;
}

void VotingPage::updateTabsAndLabels()
{
    setupTabChain(0);
}

QWidget *VotingPage::setupTabChain(QWidget *prev)
{
    QWidget::setTabOrder(prev, ui->voteYesButton);
    QWidget::setTabOrder(ui->voteNoButton, ui->voteYesButton);

    return ui->voteNoButton;
}



SendVotingConfirmationDialog::SendVotingConfirmationDialog(const QString &title, const QString &text, int _secDelay,
    QWidget *parent) :
    QMessageBox(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::Cancel, parent),
	locAmount(0),
	secDelay(_secDelay)

{
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    updateYesButton();
    connect(&countDownTimer, SIGNAL(timeout()), this, SLOT(countDown()));
}

int SendVotingConfirmationDialog::exec()
{
    QHBoxLayout* hlayout = new QHBoxLayout(qobject_cast<QWidget*>(this->children()[2]));
    locAmount = new QLineEdit(qobject_cast<QWidget*>(this->children()[2]));
    locAmount->setFixedWidth(200);
    locAmount->setPlaceholderText(tr("number of LOCs to burn"));
    locAmount->setText(QString::number(0.00000000, 'f', 8));

    QLocale lo(QLocale::C);
    lo.setNumberOptions(QLocale::RejectGroupSeparator);
    QDoubleValidator *validator = new QDoubleValidator(this);
    validator->setBottom(0.00000000);
    validator->setDecimals(8);
    validator->setTop(1000.00000000);
    validator->setLocale(lo);
    validator->setNotation(QDoubleValidator::StandardNotation);
    locAmount->setValidator(validator);

	connect(locAmount, SIGNAL(textChanged(const QString &)), this, SLOT(customSlot(const QString &)));

    hlayout->setSpacing(20);
    QSpacerItem* spacer = new QSpacerItem(40, 20, QSizePolicy::Fixed, QSizePolicy::Minimum);
    //
    hlayout->addSpacerItem(spacer);
    hlayout->setSpacing(20);
    hlayout->addWidget(locAmount);
    updateYesButton();
    countDownTimer.start(1000);
    return QMessageBox::exec();
}

void SendVotingConfirmationDialog::setText(const QString& text)
{
	QMessageBox::setTextFormat(Qt::TextFormat::RichText);
	QMessageBox::setText(text);
}

void SendVotingConfirmationDialog::customSlot(const QString& text)
{
	if(locAmount->text().toDouble() == 0)
	{
		yesButton->setEnabled(false);
		yesButton->setText(tr("Yes") + " (No LOCs)");
	}
	else
	{
		yesButton->setEnabled(true);
		yesButton->setText(tr("Yes"));
	}
}

void SendVotingConfirmationDialog::countDown()
{
    secDelay--;
    updateYesButton();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void SendVotingConfirmationDialog::updateYesButton()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(tr("Yes") + " (" + QString::number(secDelay) + ")");
    }
    else
    {
        yesButton->setEnabled(true);
        yesButton->setText(tr("Yes"));
    	if(locAmount->text().toDouble() == 0)
    	{
    		yesButton->setEnabled(false);
    		yesButton->setText(tr("Yes") + " (No LOCs)");
    	}
    }
}
