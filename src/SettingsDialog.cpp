/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010-2011, Christian Muehlhaeuser <muesli@tomahawk-player.org>
 *   Copyright 2010-2011, Leo Franchi <lfranchi@kde.org>
 *   Copyright 2010-2012, Jeff Mitchell <jeff@tomahawk-player.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "SettingsDialog.h"
#include "config.h"

#include <QtGui/QDesktopServices>
#include <QtGui/QFileDialog>
#include <QtGui/QMessageBox>
#include <QtNetwork/QNetworkConfiguration>
#include <QtNetwork/QNetworkProxy>
#include <QtGui/QVBoxLayout>
#include <QtGui/QSizeGrip>

#include "AtticaManager.h"
#include "TomahawkApp.h"
#include "TomahawkSettings.h"
#include "DelegateConfigWrapper.h"
#include "MusicScanner.h"
#include "Pipeline.h"
#include "Resolver.h"
#include "ExternalResolverGui.h"
#include "utils/tomahawkutilsgui.h"
#include "guihelpers.h"
#include "ScanManager.h"
#include "SettingsListDelegate.h"
#include "AccountDelegate.h"
#include "database/database.h"
#include "network/Servent.h"
#include "playlist/dynamic/widgets/LoadingSpinner.h"
#include "accounts/AccountModel.h"
#include "accounts/Account.h"
#include "accounts/AccountManager.h"
#include <accounts/AccountModelFilterProxy.h>
#include <accounts/ResolverAccount.h>
#include "utils/logger.h"
#include "AccountFactoryWrapper.h"

#include "ui_ProxyDialog.h"
#include "ui_StackedSettingsDialog.h"

using namespace Tomahawk;
using namespace Accounts;

SettingsDialog::SettingsDialog( QWidget *parent )
    : QDialog( parent )
    , ui( new Ui_StackedSettingsDialog )
    , m_proxySettings( this )
    , m_rejected( false )
    , m_accountModel( 0 )
    , m_sipSpinner( 0 )
{
    ui->setupUi( this );
    TomahawkSettings* s = TomahawkSettings::instance();

    TomahawkUtils::unmarginLayout( layout() );
    ui->stackedWidget->setContentsMargins( 4, 4, 4, 0 );

    ui->checkBoxReporter->setChecked( s->crashReporterEnabled() );
    ui->checkBoxHttp->setChecked( s->httpEnabled() );
    ui->checkBoxStaticPreferred->setChecked( s->preferStaticHostPort() );
    ui->checkBoxUpnp->setChecked( s->externalAddressMode() == TomahawkSettings::Upnp );
    ui->checkBoxUpnp->setEnabled( !s->preferStaticHostPort() );

    createIcons();
#ifdef Q_WS_X11
    ui->listWidget->setFrameShape( QFrame::StyledPanel );
    ui->listWidget->setFrameShadow( QFrame::Sunken );
    setContentsMargins( 4, 4, 4, 4 );
#else
    setContentsMargins( 0, 4, 4, 4 );
#endif

#ifdef Q_WS_MAC
    ui->listWidget->setFixedWidth( 83 );
#endif

#ifdef Q_WS_MAC
    // Avoid resize handles on sheets on osx
    m_proxySettings.setSizeGripEnabled( true );
    QSizeGrip* p = m_proxySettings.findChild< QSizeGrip* >();
    p->setFixedSize( 0, 0 );
#endif

    // Accounts
    AccountDelegate* accountDelegate = new AccountDelegate( this );
    ui->accountsView->setItemDelegate( accountDelegate );
    ui->accountsView->setContextMenuPolicy( Qt::CustomContextMenu );
    ui->accountsView->setVerticalScrollMode( QAbstractItemView::ScrollPerPixel );
    ui->accountsView->setMouseTracking( true );

    connect( accountDelegate, SIGNAL( openConfig( Tomahawk::Accounts::Account* ) ), this, SLOT( openAccountConfig( Tomahawk::Accounts::Account* ) ) );
    connect( accountDelegate, SIGNAL( openConfig( Tomahawk::Accounts::AccountFactory* ) ), this, SLOT( openAccountFactoryConfig( Tomahawk::Accounts::AccountFactory* ) ) );
    connect( accountDelegate, SIGNAL( update( QModelIndex ) ), ui->accountsView, SLOT( update( QModelIndex ) ) );

    m_accountModel = new AccountModel( this );
    m_accountProxy = new AccountModelFilterProxy( m_accountModel );
    m_accountProxy->setSourceModel( m_accountModel );

    connect( m_accountProxy, SIGNAL( scrollTo( QModelIndex ) ), this, SLOT( scrollTo( QModelIndex ) ) );

    ui->accountsView->setModel( m_accountProxy );

    connect( ui->installFromFileBtn, SIGNAL( clicked( bool ) ), this, SLOT( installFromFile() ) );
    connect( m_accountModel, SIGNAL( createAccount( Tomahawk::Accounts::AccountFactory* ) ), this, SLOT( createAccountFromFactory( Tomahawk::Accounts::AccountFactory* ) ) );

    ui->accountsFilterCombo->addItem( tr( "All" ), Accounts::NoType );
    ui->accountsFilterCombo->addItem( accountTypeToString( SipType ), SipType );
    ui->accountsFilterCombo->addItem( accountTypeToString( ResolverType ), ResolverType );
    ui->accountsFilterCombo->addItem( accountTypeToString( StatusPushType ), StatusPushType );

    connect( ui->accountsFilterCombo, SIGNAL( activated( int ) ), this, SLOT( accountsFilterChanged( int ) ) );

    if ( !Servent::instance()->isReady() )
    {
        m_sipSpinner = new LoadingSpinner( ui->accountsView );
        m_sipSpinner->fadeIn();

        connect( Servent::instance(), SIGNAL( ready() ), this, SLOT( serventReady() ) );
    }

    // ADVANCED
    ui->staticHostName->setText( s->externalHostname() );
    ui->staticPort->setValue( s->externalPort() );
    ui->proxyButton->setVisible( true );

    ui->checkBoxWatchForChanges->setChecked( s->watchForChanges() );
    ui->scannerTimeSpinBox->setValue( s->scannerTime() );
    ui->enableEchonestCatalog->setChecked( s->enableEchonestCatalogs() );

    connect( ui->checkBoxWatchForChanges, SIGNAL( clicked( bool ) ), SLOT( updateScanOptionsView() ) );

    if ( ui->checkBoxWatchForChanges->isChecked() )
    {
        ui->scanTimeLabel->show();
        ui->scannerTimeSpinBox->show();
    }
    else
    {
        ui->scanTimeLabel->hide();
        ui->scannerTimeSpinBox->hide();
    }

    foreach ( const QString& dir, TomahawkSettings::instance()->scannerPaths() )
    {
        ui->dirTree->checkPath( dir, Qt::Checked );
    }

    // NOW PLAYING
// #ifdef Q_WS_MAC
//     ui->checkBoxEnableAdium->setChecked( s->nowPlayingEnabled() );
// #else
//     ui->checkBoxEnableAdium->hide();
// #endif

    connect( ui->proxyButton,  SIGNAL( clicked() ),  SLOT( showProxySettings() ) );
    connect( ui->checkBoxStaticPreferred, SIGNAL( toggled(bool) ), SLOT( toggleUpnp(bool) ) );
    connect( ui->checkBoxStaticPreferred, SIGNAL( toggled(bool) ), SLOT( requiresRestart() ) );
    connect( ui->checkBoxUpnp, SIGNAL( toggled(bool) ), SLOT( requiresRestart() ) );
    connect( ui->checkBoxReporter, SIGNAL( toggled(bool) ), SLOT( requiresRestart() ) );
    connect( this, SIGNAL( rejected() ), SLOT( onRejected() ) );

    ui->listWidget->setCurrentRow( 0 );
    ui->listWidget->setItemDelegate(new SettingsListDelegate());
}


SettingsDialog::~SettingsDialog()
{
    qDebug() << Q_FUNC_INFO;

    if ( !m_rejected )
    {
        TomahawkSettings* s = TomahawkSettings::instance();

        s->setCrashReporterEnabled( ui->checkBoxReporter->checkState() == Qt::Checked );
        s->setHttpEnabled( ui->checkBoxHttp->checkState() == Qt::Checked );
        s->setPreferStaticHostPort( ui->checkBoxStaticPreferred->checkState() == Qt::Checked );
        s->setExternalAddressMode( ui->checkBoxUpnp->checkState() == Qt::Checked ? TomahawkSettings::Upnp : TomahawkSettings::Lan );

        s->setExternalHostname( ui->staticHostName->text() );
        s->setExternalPort( ui->staticPort->value() );

        s->setScannerPaths( ui->dirTree->getCheckedPaths() );
        s->setWatchForChanges( ui->checkBoxWatchForChanges->isChecked() );
        s->setScannerTime( ui->scannerTimeSpinBox->value() );
        s->setEnableEchonestCatalogs( ui->enableEchonestCatalog->isChecked() );

//         s->setNowPlayingEnabled( ui->checkBoxEnableAdium->isChecked() );

        s->applyChanges();
        s->sync();
    }
    else
        qDebug() << "Settings dialog cancelled, NOT saving prefs.";

    delete ui;
}


void
SettingsDialog::serventReady()
{
    m_sipSpinner->fadeOut();
}


void
SettingsDialog::createIcons()
{
    /// Not fun but QListWidget sucks. Do our max-width calculation manually
    /// so the icons arre lined up.
    // Resolvers is the longest string... in english. fml.

    ensurePolished();

    int maxlen = 0;
    QFontMetrics fm( font() );
    QListWidgetItem *accountsButton = new QListWidgetItem( ui->listWidget );
    accountsButton->setIcon( QIcon( RESPATH "images/account-settings.png" ) );
    accountsButton->setText( tr( "Services" ) );
    accountsButton->setTextAlignment( Qt::AlignHCenter );
    accountsButton->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled );
    maxlen = fm.width( accountsButton->text() );

    QListWidgetItem *musicButton = new QListWidgetItem( ui->listWidget );
    musicButton->setIcon( QIcon( RESPATH "images/music-settings.png" ) );
    musicButton->setText( tr( "Collection" ) );
    musicButton->setTextAlignment( Qt::AlignHCenter );
    musicButton->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled );
    maxlen = qMax( fm.width( musicButton->text() ), maxlen );

    QListWidgetItem *advancedButton = new QListWidgetItem( ui->listWidget );
    advancedButton->setIcon( QIcon( RESPATH "images/advanced-settings.png" ) );
    advancedButton->setText( tr( "Advanced" ) );
    advancedButton->setTextAlignment( Qt::AlignHCenter );
    advancedButton->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled );
    maxlen = qMax( fm.width( advancedButton->text() ), maxlen );

    maxlen += 15; // padding
    accountsButton->setSizeHint( QSize( maxlen, 60 ) );
    musicButton->setSizeHint( QSize( maxlen, 60 ) );
    advancedButton->setSizeHint( QSize( maxlen, 60 ) );

#ifndef Q_WS_MAC
    // doesn't listen to sizehint...
    ui->listWidget->setFixedWidth( maxlen + 8 );
#endif

    connect( ui->listWidget, SIGNAL( currentItemChanged( QListWidgetItem*, QListWidgetItem* ) ), SLOT( changePage( QListWidgetItem*, QListWidgetItem* ) ) );
}


void
SettingsDialog::changePage( QListWidgetItem* current, QListWidgetItem* previous )
{
    if ( !current )
        current = previous;

    ui->stackedWidget->setCurrentIndex( ui->listWidget->row(current) );
}


void
SettingsDialog::onRejected()
{
    m_rejected = true;
}


void
SettingsDialog::changeEvent( QEvent *e )
{
    QDialog::changeEvent( e );
    switch ( e->type() )
    {
        case QEvent::LanguageChange:
            ui->retranslateUi( this );
            break;

        default:
            break;
    }
}


void
SettingsDialog::showProxySettings()
{
    m_proxySettings.exec();
    if ( m_proxySettings.result() == QDialog::Accepted )
        m_proxySettings.saveSettings();
}


void
SettingsDialog::toggleUpnp( bool preferStaticEnabled )
{
    if ( preferStaticEnabled )
        ui->checkBoxUpnp->setEnabled( false );
    else
        ui->checkBoxUpnp->setEnabled( true );
}


void
SettingsDialog::updateScanOptionsView()
{
    if ( ui->checkBoxWatchForChanges->isChecked() )
    {
        ui->scanTimeLabel->show();
        ui->scannerTimeSpinBox->show();
    }
    else
    {
        ui->scanTimeLabel->hide();
        ui->scannerTimeSpinBox->hide();
    }
}


void
SettingsDialog::accountsFilterChanged( int )
{
    AccountType filter = static_cast< AccountType >( ui->accountsFilterCombo->itemData( ui->accountsFilterCombo->currentIndex() ).toInt() );
    m_accountProxy->setFilterType( filter );
}


void
SettingsDialog::openAccountFactoryConfig( AccountFactory* factory )
{
    QList< Account* > accts;
    foreach ( Account* acct, AccountManager::instance()->accounts() )
    {
        if ( AccountManager::instance()->factoryForAccount( acct ) == factory )
            accts << acct;
        if ( accts.size() > 1 )
            break;
    }
    Q_ASSERT( accts.size() > 0 ); // Shouldn't have a config wrench if there are no accounts!
    if ( accts.size() == 1 )
    {
        // If there's just one, open the config directly w/ the delete button. Otherwise open the multi dialog
        openAccountConfig( accts.first(), true );
        return;
    }

#ifndef Q_OS_MAC
    AccountFactoryWrapper dialog( factory, this );
    QWeakPointer< AccountFactoryWrapper > watcher( &dialog );

    dialog.exec();
#else
    // on osx a sheet needs to be non-modal
    AccountFactoryWrapper* dialog = new AccountFactoryWrapper( factory, this );
    dialog->show();
#endif
}


void
SettingsDialog::createAccountFromFactory( AccountFactory* factory )
{
    TomahawkUtils::createAccountFromFactory( factory, this );
}


void
SettingsDialog::openAccountConfig( Account* account, bool showDelete )
{
    TomahawkUtils::openAccountConfig( account, this, showDelete );
}


void
SettingsDialog::installFromFile()
{
    const QString resolver = QFileDialog::getOpenFileName( this, tr( "Install resolver from file" ), TomahawkSettings::instance()->scriptDefaultPath() );

    if( !resolver.isEmpty() )
    {
        Account* acct = AccountManager::instance()->accountFromPath( resolver );

        AccountManager::instance()->addAccount( acct );
        TomahawkSettings::instance()->addAccount( acct->accountId() );
        AccountManager::instance()->enableAccount( acct );


        QFileInfo resolverAbsoluteFilePath( resolver );
        TomahawkSettings::instance()->setScriptDefaultPath( resolverAbsoluteFilePath.absolutePath() );
    }
}


void
SettingsDialog::scrollTo( const QModelIndex& idx )
{
    ui->accountsView->scrollTo( idx, QAbstractItemView::PositionAtBottom );
}


void
SettingsDialog::requiresRestart()
{
    QMessageBox::information( this, tr( "Information" ), tr( "Changing this setting requires a restart of Tomahawk!" ) );
}


ProxyDialog::ProxyDialog( QWidget *parent )
: QDialog( parent )
, ui( new Ui::ProxyDialog )
{
    ui->setupUi( this );

    // ugly, I know, but...

    int i = 0;
    ui->typeBox->insertItem( i, "No Proxy", QNetworkProxy::NoProxy );
    m_forwardMap[ QNetworkProxy::NoProxy ] = i;
    m_backwardMap[ i ] = QNetworkProxy::NoProxy;
    i++;
    ui->typeBox->insertItem( i, "SOCKS 5", QNetworkProxy::Socks5Proxy );
    m_forwardMap[ QNetworkProxy::Socks5Proxy ] = i;
    m_backwardMap[ i ] = QNetworkProxy::Socks5Proxy;
    i++;

    TomahawkSettings* s = TomahawkSettings::instance();

    ui->typeBox->setCurrentIndex( m_forwardMap[s->proxyType()] );
    ui->hostLineEdit->setText( s->proxyHost() );
    ui->portSpinBox->setValue( s->proxyPort() );
    ui->userLineEdit->setText( s->proxyUsername() );
    ui->passwordLineEdit->setText( s->proxyPassword() );
    ui->checkBoxUseProxyForDns->setChecked( s->proxyDns() );
    ui->noHostLineEdit->setText( s->proxyNoProxyHosts() );

    if ( s->proxyType() == QNetworkProxy::NoProxy )
    {
        ui->hostLineEdit->setEnabled( false );
        ui->portSpinBox->setEnabled( false );
        ui->userLineEdit->setEnabled( false );
        ui->passwordLineEdit->setEnabled( false );
        ui->checkBoxUseProxyForDns->setEnabled( false );
    }

    connect( ui->typeBox, SIGNAL( currentIndexChanged( int ) ), SLOT( proxyTypeChangedSlot( int ) ) );
}


void
ProxyDialog::proxyTypeChangedSlot( int index )
{
    if ( m_backwardMap[ index ] == QNetworkProxy::NoProxy )
    {
        ui->hostLineEdit->setEnabled( false );
        ui->portSpinBox->setEnabled( false );
        ui->userLineEdit->setEnabled( false );
        ui->passwordLineEdit->setEnabled( false );
        ui->checkBoxUseProxyForDns->setEnabled( false );
    }
    else
    {
        ui->hostLineEdit->setEnabled( true );
        ui->portSpinBox->setEnabled( true );
        ui->userLineEdit->setEnabled( true );
        ui->passwordLineEdit->setEnabled( true );
        ui->checkBoxUseProxyForDns->setEnabled( true );
    }
}


void
ProxyDialog::saveSettings()
{
    qDebug() << Q_FUNC_INFO;

    QNetworkProxy::ProxyType type = static_cast< QNetworkProxy::ProxyType>( m_backwardMap[ ui->typeBox->currentIndex() ] );

    //First set settings
    TomahawkSettings* s = TomahawkSettings::instance();
    s->setProxyHost( ui->hostLineEdit->text() );

    int port = ui->portSpinBox->value();
    s->setProxyPort( port );
    s->setProxyNoProxyHosts( ui->noHostLineEdit->text() );
    s->setProxyUsername( ui->userLineEdit->text() );
    s->setProxyPassword( ui->passwordLineEdit->text() );
    s->setProxyType( type );
    s->setProxyDns( ui->checkBoxUseProxyForDns->checkState() == Qt::Checked );
    s->sync();

    TomahawkUtils::NetworkProxyFactory* proxyFactory = TomahawkUtils::proxyFactory();
    tDebug() << Q_FUNC_INFO << "Got proxyFactory: " << proxyFactory;
    if ( type == QNetworkProxy::NoProxy )
    {
        tDebug() << Q_FUNC_INFO << "Got NoProxy selected";
        proxyFactory->setProxy( QNetworkProxy::NoProxy );
    }
    else
    {
        tDebug() << Q_FUNC_INFO << "Got Socks5Proxy selected";
        proxyFactory->setProxy( QNetworkProxy( type, s->proxyHost(), s->proxyPort(), s->proxyUsername(), s->proxyPassword() ) );
        if ( !ui->noHostLineEdit->text().isEmpty() )
        {
            tDebug() << Q_FUNC_INFO << "noproxy line edit is " << ui->noHostLineEdit->text();
            tDebug() << Q_FUNC_INFO << "split noproxy line edit is " << ui->noHostLineEdit->text().split( ' ', QString::SkipEmptyParts );
            proxyFactory->setNoProxyHosts( ui->noHostLineEdit->text().split( ' ', QString::SkipEmptyParts ) );
        }
    }
}
