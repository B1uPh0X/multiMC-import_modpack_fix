#include <QNetworkAccessManager>
#include <QNetworkReply>

#include "katabasis/PollServer.h"
#include "JsonResponse.h"

namespace {
QMap<QString, QString> toVerificationParams(const QVariantMap &map)
{
    QMap<QString, QString> params;
    for (QVariantMap::const_iterator i = map.constBegin();
         i != map.constEnd(); ++i)
    {
        params[i.key()] = i.value().toString();
    }
    return params;
}
}

namespace Katabasis {

PollServer::PollServer(QNetworkAccessManager *manager, const QNetworkRequest &request, const QByteArray &payload, int expiresIn, QObject *parent)
    : QObject(parent)
    , manager_(manager)
    , request_(request)
    , payload_(payload)
    , expiresIn_(expiresIn)
{
    expirationTimer.setTimerType(Qt::VeryCoarseTimer);
    expirationTimer.setInterval(expiresIn * 1000);
    expirationTimer.setSingleShot(true);
    connect(&expirationTimer, SIGNAL(timeout()), this, SLOT(onExpiration()));
    expirationTimer.start();

    pollTimer.setTimerType(Qt::VeryCoarseTimer);
    pollTimer.setInterval(5 * 1000);
    pollTimer.setSingleShot(true);
    connect(&pollTimer, SIGNAL(timeout()), this, SLOT(onPollTimeout()));
}

int PollServer::interval() const
{
    return pollTimer.interval() / 1000;
}

void PollServer::setInterval(int interval)
{
    pollTimer.setInterval(interval * 1000);
}

void PollServer::startPolling()
{
    if (expirationTimer.isActive()) {
        pollTimer.start();
    }
}

void PollServer::onPollTimeout()
{
    QNetworkReply * reply = manager_->post(request_, payload_);
    connect(reply, SIGNAL(finished()), this, SLOT(onReplyFinished()));
}

void PollServer::onExpiration()
{
    pollTimer.stop();
    emit serverClosed(false);
}

void PollServer::onReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());

    if (!reply) {
        qDebug() << "PollServer::onReplyFinished: reply is null";
        return;
    }

    QByteArray replyData = reply->readAll();
    QMap<QString, QString> params = toVerificationParams(parseJsonResponse(replyData));

    // Dump replyData
    // SENSITIVE DATA in RelWithDebInfo or Debug builds
    // qDebug() << "PollServer::onReplyFinished: replyData\n";
    // qDebug() << QString( replyData );

    if (reply->error() == QNetworkReply::TimeoutError) {
        // rfc8628#section-3.2
        // "On encountering a connection timeout, clients MUST unilaterally
        // reduce their polling frequency before retrying.  The use of an
        // exponential backoff algorithm to achieve this, such as doubling the
        // polling interval on each such connection timeout, is RECOMMENDED."
        setInterval(interval() * 2);
        pollTimer.start();
    }
    else {
        QString error = params.value("error");
        if (error == "slow_down") {
            // rfc8628#section-3.2
            // "A variant of 'authorization_pending', the authorization request is
            // still pending and polling should continue, but the interval MUST
            // be increased by 5 seconds for this and all subsequent requests."
            setInterval(interval() + 5);
            pollTimer.start();
        }
        else if (error == "authorization_pending") {
            // keep trying - rfc8628#section-3.2
            // "The authorization request is still pending as the end user hasn't
            // yet completed the user-interaction steps (Section 3.3)."
            pollTimer.start();
        }
        else {
            expirationTimer.stop();
            emit serverClosed(true);
            // let O2 handle the other cases
            emit verificationReceived(params);
        }
    }
    reply->deleteLater();
}

}
