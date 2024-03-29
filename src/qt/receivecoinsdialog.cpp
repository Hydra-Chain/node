// Copyright (c) 2011-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <qt/receivecoinsdialog.h>
#include <qt/forms/ui_receivecoinsdialog.h>

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>
#include <qt/styleSheet.h>

#include <QAction>
#include <QCursor>
#include <QMessageBox>
#include <QScrollBar>
#include <QTextDocument>
#include <QSettings>

ReceiveCoinsDialog::ReceiveCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ReceiveCoinsDialog),
    columnResizingFixer(0),
    model(0),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    // Set stylesheet
    SetObjectStyleSheet(ui->clearButton, StyleSheetNames::ButtonBlack);
    SetObjectStyleSheet(ui->showRequestButton, StyleSheetNames::ButtonTransparentBordered);
    SetObjectStyleSheet(ui->removeRequestButton, StyleSheetNames::ButtonTransparentBordered);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->clearButton->setIcon(QIcon());
        ui->receiveButton->setIcon(QIcon());
        ui->showRequestButton->setIcon(QIcon());
        ui->removeRequestButton->setIcon(QIcon());
    } else {
        ui->clearButton->setIcon(_platformStyle->MultiStatesIcon(":/icons/remove", PlatformStyle::PushButton));
        ui->receiveButton->setIcon(_platformStyle->MultiStatesIcon(":/icons/request_payment", PlatformStyle::PushButton));
        ui->showRequestButton->setIcon(_platformStyle->MultiStatesIcon(":/icons/show", PlatformStyle::PushButton));
        ui->removeRequestButton->setIcon(_platformStyle->MultiStatesIcon(":/icons/remove", PlatformStyle::PushButton));
    }

    ui->copyAddressButton->setIcon(platformStyle->MultiStatesIcon(":/icons/editcopy", PlatformStyle::PushButton));
    ui->copyAddressButton->setEnabled(false);
    ui->refreshButton->setIcon(platformStyle->MultiStatesIcon(":/movies/spinner-010", PlatformStyle::PushButton));
    ui->refreshButton->setVisible(false);
    ui->leAddress->setReadOnly(true);

    GUIUtil::formatToolButtons(ui->copyAddressButton, ui->refreshButton);

    // context menu actions
    QAction *copyURIAction = new QAction(tr("Copy URI"), this);
    QAction *copyLabelAction = new QAction(tr("Copy label"), this);
    QAction *copyMessageAction = new QAction(tr("Copy message"), this);
    QAction *copyAmountAction = new QAction(tr("Copy amount"), this);

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(copyURIAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(copyMessageAction);
    contextMenu->addAction(copyAmountAction);

    // context menu signals
    connect(ui->recentRequestsView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(showMenu(QPoint)));
    connect(copyURIAction, SIGNAL(triggered()), this, SLOT(copyURI()));
    connect(copyLabelAction, SIGNAL(triggered()), this, SLOT(copyLabel()));
    connect(copyMessageAction, SIGNAL(triggered()), this, SLOT(copyMessage()));
    connect(copyAmountAction, SIGNAL(triggered()), this, SLOT(copyAmount()));

    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));
}

void ReceiveCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        _model->getRecentRequestsTableModel()->sort(RecentRequestsTableModel::Date, Qt::DescendingOrder);
        connect(_model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
        updateDisplayUnit();

        QTableView* tableView = ui->recentRequestsView;

        tableView->verticalHeader()->hide();
        tableView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tableView->setModel(_model->getRecentRequestsTableModel());
        tableView->setAlternatingRowColors(true);
        tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableView->setSelectionMode(QAbstractItemView::ContiguousSelection);
        tableView->setColumnWidth(RecentRequestsTableModel::Date, DATE_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Label, LABEL_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);

        connect(tableView->selectionModel(),
            SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this,
            SLOT(recentRequestsView_selectionChanged(QItemSelection, QItemSelection)));
        // Last 2 columns are set by the columnResizingFixer, when the table geometry is ready.
        columnResizingFixer = new GUIUtil::TableViewLastColumnResizingFixer(tableView, AMOUNT_MINIMUM_COLUMN_WIDTH, DATE_COLUMN_WIDTH, this);

        if (model->wallet().getDefaultAddressType() == OutputType::BECH32) {
            ui->useLegacyAddress->setCheckState(Qt::Unchecked);
        } else {
            ui->useLegacyAddress->setCheckState(Qt::Checked);
        }

        ui->useLegacyAddress->setVisible(model->wallet().getDefaultAddressType() != OutputType::LEGACY);

        // eventually disable the main receive button if private key operations are disabled
        ui->receiveButton->setEnabled(!model->privateKeysDisabled());

		QSettings settings;
        if (settings.contains("nReceiveDefaultAddress") && settings.value("nReceiveDefaultAddress").toString() != "")
        {
        	ui->defaultAddress->setText(settings.value("nReceiveDefaultAddress").toString());
        	ui->leAddress->setText(settings.value("nReceiveDefaultAddress").toString());
        }
        if(ui->defaultAddress->text() == "" || !model->wallet().isMineAddress(ui->defaultAddress->text().toStdString()))
        {
            QVariant addr = model->getAddressTableModel()->data(model->getAddressTableModel()->index(0, 1, QModelIndex()), Qt::DisplayRole);
            ui->defaultAddress->setText(addr.toString());
            settings.setValue("nReceiveDefaultAddress", addr.toString());
            ui->leAddress->setText(addr.toString());
        }
    }
}

ReceiveCoinsDialog::~ReceiveCoinsDialog()
{
    delete ui;
}

void ReceiveCoinsDialog::clear()
{
    ui->reqAmount->clear();
    ui->reqLabel->setText("");
    ui->reqMessage->setText("");
    ui->leAddress->setText("");
    ui->copyAddressButton->setEnabled(false);
    QPixmap emptyPixmap;
    ui->lblQRCode->setPixmap(emptyPixmap);
    ui->lblQRCode->setText("");
    updateDisplayUnit();
}

void ReceiveCoinsDialog::reject()
{
    clear();
}

void ReceiveCoinsDialog::accept()
{
    clear();
}

void ReceiveCoinsDialog::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        ui->reqAmount->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    }
}

void ReceiveCoinsDialog::on_receiveButton_clicked()
{
    if(!model || !model->getOptionsModel() || !model->getAddressTableModel() || !model->getRecentRequestsTableModel())
        return;

    QString address;
    QString label = ui->reqLabel->text();

	OutputType address_type;
    if(ui->reuseDefaultAddress->isChecked())
    {
    	address = ui->defaultAddress->text();
    	label = model->getAddressTableModel()->labelForAddress(address);
    }
    else if(ui->reuseExistingAddress->isChecked())
    {
        /* Use selected address*/
        if(ui->leAddress->text() != "")
        {
            address = ui->leAddress->text();
        } else {
            /* Choose existing receiving address */
            AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
            dlg.setModel(model->getAddressTableModel());
            if(dlg.exec())
            {
                address = dlg.getReturnValue();
                if(label.isEmpty()) /* If no label provided, use the previously used label */
                {
                   label = model->getAddressTableModel()->labelForAddress(address);
                }
            } else {
                return;
            }
        }
    } else {
        /* Generate new receiving address */
		if (!ui->useLegacyAddress->isChecked()) {
	        address_type = OutputType::BECH32;
	    } else {
	        address_type = model->wallet().getDefaultAddressType();
	        if (address_type == OutputType::BECH32) {
	            address_type = OutputType::P2SH_SEGWIT;
	        }
	    }
    	address = model->getAddressTableModel()->addRow(AddressTableModel::Receive, label, "", address_type);
    }

    SendCoinsRecipient info(address, label,
        ui->reqAmount->value(), ui->reqMessage->text());
    ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModel(model);
    dialog->setInfo(info);
    dialog->show();
    clear();

    /* Store request for later reference */
    model->getRecentRequestsTableModel()->addNewRequest(info);
}

void ReceiveCoinsDialog::on_recentRequestsView_doubleClicked(const QModelIndex &index)
{
    const RecentRequestsTableModel *submodel = model->getRecentRequestsTableModel();
    ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
    dialog->setModel(model);
    dialog->setInfo(submodel->entry(index.row()).recipient);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void ReceiveCoinsDialog::on_recentRequestsView_clicked(const QModelIndex &index)
{
    const RecentRequestsTableModel *submodel = model->getRecentRequestsTableModel();
    SendCoinsRecipient info = submodel->entry(index.row()).recipient;

    ui->leAddress->setText(info.address);
    ui->reqLabel->setText(info.label);
    ui->reqMessage->setText(info.message);
    ui->reqAmount->setValue(info.amount);

    if(ReceiveRequestDialog::createQRCode(ui->lblQRCode, info))
    {
        ui->lblQRCode->setScaledContents(true);
    }

    ui->copyAddressButton->setEnabled(true);
}

void ReceiveCoinsDialog::recentRequestsView_selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    // Enable Show/Remove buttons only if anything is selected.
    bool enable = !ui->recentRequestsView->selectionModel()->selectedRows().isEmpty();
    ui->showRequestButton->setEnabled(enable);
    ui->removeRequestButton->setEnabled(enable);
}

void ReceiveCoinsDialog::on_showRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();

    for (const QModelIndex& index : selection) {
        on_recentRequestsView_doubleClicked(index);
    }
}

void ReceiveCoinsDialog::on_removeRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return;
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    model->getRecentRequestsTableModel()->removeRows(firstIndex.row(), selection.length(), firstIndex.parent());
}

void ReceiveCoinsDialog::on_copyAddressButton_clicked()
{
    GUIUtil::setClipboard(ui->leAddress->text());
}

// We override the virtual resizeEvent of the QWidget to adjust tables column
// sizes as the tables width is proportional to the dialogs width.
void ReceiveCoinsDialog::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    columnResizingFixer->stretchColumnWidth(RecentRequestsTableModel::Message);
}

void ReceiveCoinsDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return)
    {
        // press return -> submit form
        if (ui->reqLabel->hasFocus() || ui->reqAmount->hasFocus() || ui->reqMessage->hasFocus())
        {
            event->ignore();
            on_receiveButton_clicked();
            return;
        }
    }

    this->QDialog::keyPressEvent(event);
}

QModelIndex ReceiveCoinsDialog::selectedRow()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return QModelIndex();
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return QModelIndex();
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    return firstIndex;
}

// copy column of selected row to clipboard
void ReceiveCoinsDialog::copyColumnToClipboard(int column)
{
    QModelIndex firstIndex = selectedRow();
    if (!firstIndex.isValid()) {
        return;
    }
    GUIUtil::setClipboard(model->getRecentRequestsTableModel()->data(firstIndex.child(firstIndex.row(), column), Qt::EditRole).toString());
}

// context menu
void ReceiveCoinsDialog::showMenu(const QPoint &point)
{
    if (!selectedRow().isValid()) {
        return;
    }
    contextMenu->exec(QCursor::pos());
}

// context menu action: copy URI
void ReceiveCoinsDialog::copyURI()
{
    QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    const RecentRequestsTableModel * const submodel = model->getRecentRequestsTableModel();
    const QString uri = GUIUtil::formatBitcoinURI(submodel->entry(sel.row()).recipient);
    GUIUtil::setClipboard(uri);
}

// context menu action: copy label
void ReceiveCoinsDialog::copyLabel()
{
    copyColumnToClipboard(RecentRequestsTableModel::Label);
}

// context menu action: copy message
void ReceiveCoinsDialog::copyMessage()
{
    copyColumnToClipboard(RecentRequestsTableModel::Message);
}

// context menu action: copy amount
void ReceiveCoinsDialog::copyAmount()
{
    copyColumnToClipboard(RecentRequestsTableModel::Amount);
}

void ReceiveCoinsDialog::on_changeDefaultAddressButton_clicked()
{
	/* Choose existing receiving address */
	AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
	dlg.setModel(model->getAddressTableModel());
	if(dlg.exec())
	{
		QSettings settings;
		ui->defaultAddress->setText(dlg.getReturnValue());
		settings.setValue("nReceiveDefaultAddress", dlg.getReturnValue());
	} else {
	    return;
	}
}

void ReceiveCoinsDialog::on_chooseReuseAddressButton_clicked()
{
	/* Choose existing receiving address */
	AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
	dlg.setModel(model->getAddressTableModel());
	if(dlg.exec())
	{
		ui->leAddress->setText(dlg.getReturnValue());
	} else {
	    return;
	}
}

void ReceiveCoinsDialog::on_reuseExistingAddress_clicked()
{
	if(ui->reuseExistingAddress->isChecked())
	{
		ui->chooseReuseAddressButton->setEnabled(true);
	}
}

void ReceiveCoinsDialog::on_generateNewAddress_clicked()
{
	if(ui->generateNewAddress->isChecked())
	{
		ui->chooseReuseAddressButton->setEnabled(false);
	}
}

void ReceiveCoinsDialog::on_reuseDefaultAddress_clicked()
{
	if(ui->reuseDefaultAddress->isChecked())
	{
		ui->chooseReuseAddressButton->setEnabled(false);
		ui->leAddress->setText(ui->defaultAddress->text());
	}
}
