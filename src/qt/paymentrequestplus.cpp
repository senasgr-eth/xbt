// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Wraps dumb protocol buffer paymentRequest
// with some extra methods
//

#include "paymentrequestplus.h"

#include "util.h"

#include <stdexcept>

#include <openssl/x509_vfy.h>

#include <QDateTime>
#include <QDebug>
#include <QSslCertificate>

class SSLVerifyError : public std::runtime_error
{
public:
    SSLVerifyError(std::string err) : std::runtime_error(err) { }
};

bool PaymentRequestPlus::parse(const QByteArray& data)
{
    bool parseOK = paymentRequest.ParseFromArray(data.data(), data.size());
    if (!parseOK) {
        qWarning() << "PaymentRequestPlus::parse: Error parsing payment request";
        return false;
    }
    if (paymentRequest.payment_details_version() > 1) {
        qWarning() << "PaymentRequestPlus::parse: Received up-version payment details, version=" << paymentRequest.payment_details_version();
        return false;
    }

    parseOK = details.ParseFromString(paymentRequest.serialized_payment_details());
    if (!parseOK)
    {
        qWarning() << "PaymentRequestPlus::parse: Error parsing payment details";
        paymentRequest.Clear();
        return false;
    }
    return true;
}

bool PaymentRequestPlus::SerializeToString(std::string* output) const
{
    return paymentRequest.SerializeToString(output);
}

bool PaymentRequestPlus::IsInitialized() const
{
    return paymentRequest.IsInitialized();
}

bool PaymentRequestPlus::getMerchant(X509_STORE* certStore, QString& merchant) const
{
    merchant.clear();

    if (!IsInitialized())
        return false;

    const EVP_MD* digestAlgorithm = nullptr;
    if (paymentRequest.pki_type() == "x509+sha256") {
        digestAlgorithm = EVP_sha256();
    } else if (paymentRequest.pki_type() == "x509+sha1") {
        digestAlgorithm = EVP_sha1();
    } else if (paymentRequest.pki_type() == "none") {
        qWarning() << "PaymentRequestPlus::getMerchant: Payment request: pki_type == none";
        return false;
    } else {
        qWarning() << "PaymentRequestPlus::getMerchant: Payment request: unknown pki_type " << QString::fromStdString(paymentRequest.pki_type());
        return false;
    }

    payments::X509Certificates certChain;
    if (!certChain.ParseFromString(paymentRequest.pki_data())) {
        qWarning() << "PaymentRequestPlus::getMerchant: Payment request: error parsing pki_data";
        return false;
    }

    std::vector<X509*> certs;
    const QDateTime currentTime = QDateTime::currentDateTime();
    for (int i = 0; i < certChain.certificate_size(); i++) {
        QByteArray certData(certChain.certificate(i).data(), certChain.certificate(i).size());
        QSslCertificate qCert(certData, QSsl::Der);
        if (currentTime < qCert.effectiveDate() || currentTime > qCert.expiryDate()) {
            qWarning() << "PaymentRequestPlus::getMerchant: Payment request: certificate expired or not yet active: " << qCert;
            return false;
        }
#if QT_VERSION >= 0x050000
        if (qCert.isBlacklisted()) {
            qWarning() << "PaymentRequestPlus::getMerchant: Payment request: certificate blacklisted: " << qCert;
            return false;
        }
#endif
        const unsigned char* data = (const unsigned char*)certChain.certificate(i).data();
        X509* cert = d2i_X509(nullptr, &data, certChain.certificate(i).size());
        if (cert)
            certs.push_back(cert);
    }
    if (certs.empty()) {
        qWarning() << "PaymentRequestPlus::getMerchant: Payment request: empty certificate chain";
        return false;
    }

    STACK_OF(X509)* chain = sk_X509_new_null();
    for (int i = certs.size() - 1; i > 0; i--) {
        sk_X509_push(chain, certs[i]);
    }
    X509* signing_cert = certs[0];

    X509_STORE_CTX* store_ctx = X509_STORE_CTX_new();
    if (!store_ctx) {
        qWarning() << "PaymentRequestPlus::getMerchant: Payment request: error creating X509_STORE_CTX";
        return false;
    }

    char* website = nullptr;
    bool fResult = true;
    try {
        if (!X509_STORE_CTX_init(store_ctx, certStore, signing_cert, chain)) {
            int error = X509_STORE_CTX_get_error(store_ctx);
            throw SSLVerifyError(X509_verify_cert_error_string(error));
        }

        int result = X509_verify_cert(store_ctx);
        if (result != 1) {
            int error = X509_STORE_CTX_get_error(store_ctx);
            if (!(error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT &&
                  GetBoolArg("-allowselfsignedrootcertificates", DEFAULT_SELFSIGNED_ROOTCERTS))) {
                throw SSLVerifyError(X509_verify_cert_error_string(error));
            } else {
                qDebug() << "PaymentRequestPlus::getMerchant: Allowing self signed root certificate, because -allowselfsignedrootcertificates is true.";
            }
        }

        X509_NAME* certname = X509_get_subject_name(signing_cert);

        payments::PaymentRequest rcopy(paymentRequest);
        rcopy.set_signature(std::string(""));
        std::string data_to_verify;
        rcopy.SerializeToString(&data_to_verify);

        EVP_MD_CTX* ctx = EVP_MD_CTX_create();
        if (!ctx)
            throw SSLVerifyError("Failed to create EVP_MD_CTX");

        EVP_PKEY* pubkey = X509_get_pubkey(signing_cert);
        if (!EVP_VerifyInit_ex(ctx, digestAlgorithm, nullptr) ||
            !EVP_VerifyUpdate(ctx, data_to_verify.data(), data_to_verify.size()) ||
            !EVP_VerifyFinal(ctx, (const unsigned char*)paymentRequest.signature().data(),
                             (unsigned int)paymentRequest.signature().size(), pubkey)) {
            EVP_MD_CTX_destroy(ctx);

            throw SSLVerifyError("Bad signature, invalid payment request.");
        }

        EVP_MD_CTX_destroy(ctx);

        int textlen = X509_NAME_get_text_by_NID(certname, NID_commonName, nullptr, 0);
        website = new char[textlen + 1];
        if (X509_NAME_get_text_by_NID(certname, NID_commonName, website, textlen + 1) == textlen && textlen > 0) {
            merchant = website;
        } else {
            throw SSLVerifyError("Bad certificate, missing common name.");
        }
    } catch (const SSLVerifyError& err) {
        fResult = false;
        qWarning() << "PaymentRequestPlus::getMerchant: SSL error: " << err.what();
    }

    if (website)
        delete[] website;
    X509_STORE_CTX_free(store_ctx);
    for (unsigned int i = 0; i < certs.size(); i++)
        X509_free(certs[i]);

    return fResult;
}


QList<std::pair<CScript,CAmount> > PaymentRequestPlus::getPayTo() const
{
    QList<std::pair<CScript,CAmount> > result;
    for (int i = 0; i < details.outputs_size(); i++)
    {
        const unsigned char* scriptStr = (const unsigned char*)details.outputs(i).script().data();
        CScript s(scriptStr, scriptStr+details.outputs(i).script().size());

        result.append(std::make_pair(s, details.outputs(i).amount()));
    }
    return result;
}
