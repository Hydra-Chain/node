#include "titlebar.h"
#include "ui_titlebar.h"
#include "bitcoinunits.h"
#include "optionsmodel.h"
#include "tabbarinfo.h"

#include <QPixmap>
#include "platformstyle.h"
#include "locktrip/price-oracle.h"
#include "bitcoinunits.h"

//namespace TitleBar_NS {
//const int titleHeight = 50;
//}
//using namespace TitleBar_NS;

TitleBar::TitleBar(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::TitleBar),
    m_tab(0),
	dgp(new Dgp())
{
    ui->setupUi(this);
    // Set size policy
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->tabWidget->setDrawBase(false);
    ui->tabWidget->setTabsClosable(true);
    //setFixedHeight(titleHeight);
    m_iconCloseTab = platformStyle->TextColorIcon(":/icons/quit");
    ui->widgetLogo->setMinimumWidth(0);
    ui->tabWidget->setVisible(false);
}

TitleBar::~TitleBar()
{
    delete ui;
}

void TitleBar::setModel(WalletModel *_model)
{
    this->model = _model;

    setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance(),  model->getStake(),
               model->getWatchBalance(), model->getWatchUnconfirmedBalance(), model->getWatchImmatureBalance(), model->getWatchStake());

    connect(model, SIGNAL(balanceChanged(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)), this, SLOT(setBalance(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)));
}

void TitleBar::setTabBarInfo(QObject *info)
{
    if(m_tab)
    {
        m_tab->detach();
    }

    if(info && info->inherits("TabBarInfo"))
    {
        TabBarInfo* tab = (TabBarInfo*)info;
        m_tab = tab;
        m_tab->attach(ui->tabWidget, &m_iconCloseTab);
    }
}

void TitleBar::setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance, const CAmount& stake,
                                 const CAmount& watchBalance, const CAmount& watchUnconfirmedBalance, const CAmount& watchImmatureBalance, const CAmount& watchStake)
{
    Q_UNUSED(unconfirmedBalance);
    Q_UNUSED(immatureBalance);
    Q_UNUSED(watchBalance);
    Q_UNUSED(stake);
    Q_UNUSED(watchUnconfirmedBalance);
    Q_UNUSED(watchImmatureBalance);
    Q_UNUSED(watchStake);

    if(model && model->getOptionsModel())
    {
    	uint64_t nPrice;
    	PriceOracle oracle;
   	    oracle.getBytePrice(nPrice);
   	    uint64_t fiatPrice;
	 	dgp->getDgpParam(FIAT_BYTE_PRICE, fiatPrice);
	 	double rate = 0;
	 	if(balance > 0)
	 		rate = (double)nPrice / (double)fiatPrice;
	 	uint64_t fiat_balance = (balance * rate) / 1000000;

        //ui->lblBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance));
    	ui->lblBalance->setText( "<span style='font-size:15pt;'>" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance) +
    			"<br /></span><span style='font-size:9pt;'>" + BitcoinUnits::formatWithUnit(BitcoinUnits::USD, fiat_balance) +
				" - LOC/USD " + ((fiatPrice == 0) ? "N/A" : "1/" + QString::number(rate, 'f', 2).rightJustified(2, '0')) + "</span>" );
    }
}

void TitleBar::on_navigationResized(const QSize &_size)
{
    //ui->widgetLogo->setFixedWidth(_size.width());
}
