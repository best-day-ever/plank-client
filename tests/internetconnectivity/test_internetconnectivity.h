#pragma once

#include <QObject>

class ConnectivityTest : public QObject
{
    Q_OBJECT

private slots:
    void policyDefaultsAndExplicitValues();
    void invalidPolicy_data();
    void invalidPolicy();
    void disabledMakesNoProbes();
    void orderedFallbackAndPeriodicRestart();
    void noResponseRecoversWithoutInteraction();
    void deadlineDoesNotBlockUiOrLeaveChildren();
    void unavailableIsNotAnInternetOutage();
    void probeErrorCanFallBack();
    void networkErrorIsNotUtilityFailure_data();
    void networkErrorIsNotUtilityFailure();
    void restartAndStopDiscardCancelledResults();
    void systemPingCommandUsesPlatformContract();
    void windowsEchoReplies_data();
    void windowsEchoReplies();
    void windowsProbeDeadlineAndCancellation();
    void systemPingLoopback();
    void summaryChecksEveryTargetConcurrently();
    void summaryRoundTripFormats_data();
    void summaryRoundTripFormats();
    void summaryFailuresAndDeadlines();
    void summaryCancellationAndReopen();
    void statusUi();
};
